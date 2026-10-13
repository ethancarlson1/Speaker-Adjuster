"""Live delta: the mic against the console's signal, measured from the show.

A running dual-FFT transfer function between x = the console's signal as it
reaches the plugin (before any of its EQ) and y = the measurement mic, for the
show view's Delta panel. Above 0 dB the mic hears more of a frequency than the
console sends, relative to the reference band; below, less.

- Delay: GCC-PHAT (dualfft.estimate_delay) on the last 4 s of both, every 5 s.
  A delay is taken only when the two halves of those 4 s agree with it within
  0.5 ms; a new one resets the averages. Nothing is averaged until there is one.
- Frames: Hann-windowed, 1 s rounded up to a power of two, one every 0.5 s:
  the mic's samples up to the frame's end, the console's from `delay` earlier.
  A frame whose console part is quieter than -70 dBFS RMS isn't counted, so
  pauses in the music leave the average as it was.
- Average: Gxx, Gyy and Gxy per bin, exponential with a 10 s time constant.
- Display: on 24 points per octave, 20 Hz-20 kHz, smoothed to 1/6 octave:
  |H|^2 weighted by Gxx (the coherent mic power over the console's power in
  each slice), and the coherence weighted the same way. Points with coherence
  under 0.5 are unreliable (crowd, the stage, notes the music isn't playing).
  0 dB is the mean of the reliable points in the reference band.

Frames and delay searches happen at fixed sample positions (multiples of the
hop and of the search interval), so the result doesn't depend on how the
samples arrive.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .dualfft import estimate_delay
from .spectrum import log_freq_grid, smooth_power, to_db
from .sweep import next_pow2


@dataclass(frozen=True)
class LiveDeltaConfig:
    fft_seconds: float = 1.0          # rounded up to a power of two at the session rate
    hop_seconds: float = 0.5
    average_seconds: float = 10.0     # exponential time constant
    delay_window_s: float = 4.0       # the delay is searched on the last this many seconds
    delay_every_s: float = 5.0
    max_delay_s: float = 1.0
    delay_tolerance_ms: float = 0.5   # the window's halves must agree with it this closely
    floor_dbfs: float = -70.0         # console frames (and search windows) quieter than this don't count
    smoothing: float = 6.0            # 1/N octave
    points_per_octave: int = 24
    coherence_min: float = 0.5


@dataclass
class LiveDelta:
    grid: np.ndarray
    delta_db: np.ndarray       # mic vs console, 0 dB on the reference band; NaN where nothing was measured
    coherence: np.ndarray      # 0..1, NaN where nothing was measured
    valid: np.ndarray          # coherence at least coherence_min
    delay_ms: float | None     # the mic after the console; None until found
    frames: int                # frames averaged since the delay was found


class LiveDeltaTracker:
    def __init__(self, fs: float, ref_band: tuple[float, float] = (250.0, 4000.0),
                 cfg: LiveDeltaConfig = LiveDeltaConfig()):
        self.fs = fs
        self.ref_band = ref_band
        self.cfg = cfg
        self.nfft = next_pow2(int(cfg.fft_seconds * fs))
        self.hop = max(1, int(round(cfg.hop_seconds * fs)))
        self.window_len = int(round(cfg.delay_window_s * fs))
        self.delay_every = max(1, int(round(cfg.delay_every_s * fs)))
        self.max_lag = int(cfg.max_delay_s * fs)
        self.tolerance = cfg.delay_tolerance_ms * 1e-3 * fs
        self.alpha = 1.0 - math.exp(-(self.hop / fs) / cfg.average_seconds)
        self.floor = 10.0 ** (cfg.floor_dbfs / 20.0)
        self.win = np.hanning(self.nfft)
        need = self.hop + max(self.window_len, self.nfft + self.max_lag)
        self.capacity = next_pow2(need)
        self.x = np.zeros(self.capacity)
        self.y = np.zeros(self.capacity)
        self.total = 0
        self.next_frame = self.hop
        self.next_search = self.delay_every
        self.delay: int | None = None
        self.frames = 0
        bins = self.nfft // 2 + 1
        self.gxx = np.zeros(bins)
        self.gyy = np.zeros(bins)
        self.gxy = np.zeros(bins, dtype=complex)
        self.freqs = np.fft.rfftfreq(self.nfft, 1.0 / fs)
        self.grid = log_freq_grid(20.0, min(20000.0, 0.45 * fs), cfg.points_per_octave)

    # -- input --------------------------------------------------------------
    def push(self, console: np.ndarray, mic: np.ndarray) -> None:
        """Samples of both, taken at the same moments."""
        console = np.asarray(console, dtype=float)
        mic = np.asarray(mic, dtype=float)
        pos, n = 0, len(console)
        while pos < n:
            due = min(self.next_frame, self.next_search)
            take = min(n - pos, due - self.total)
            idx = (self.total + np.arange(take)) % self.capacity
            self.x[idx] = console[pos:pos + take]
            self.y[idx] = mic[pos:pos + take]
            self.total += take
            pos += take
            if self.total == self.next_search:
                self._search()
                self.next_search += self.delay_every
            if self.total == self.next_frame:
                self._frame()
                self.next_frame += self.hop

    def _span(self, buf: np.ndarray, start: int, length: int) -> np.ndarray:
        return buf[(start + np.arange(length)) % self.capacity]

    def _search(self) -> None:
        if self.total < self.window_len:
            return
        start = self.total - self.window_len
        x = self._span(self.x, start, self.window_len)
        y = self._span(self.y, start, self.window_len)
        if math.sqrt(float(np.mean(x * x))) < self.floor or not np.any(y):
            return
        half = self.window_len // 2
        d = estimate_delay(x, y, self.fs, self.cfg.max_delay_s)
        d1 = estimate_delay(x[:half], y[:half], self.fs, self.cfg.max_delay_s)
        d2 = estimate_delay(x[half:], y[half:], self.fs, self.cfg.max_delay_s)
        if abs(d1 - d) > self.tolerance or abs(d2 - d) > self.tolerance:
            return
        if self.delay is None or abs(d - self.delay) > self.tolerance:
            self.delay = d
            self.frames = 0
            self.gxx[:] = 0.0
            self.gyy[:] = 0.0
            self.gxy[:] = 0.0

    def _frame(self) -> None:
        if self.delay is None or self.total - self.delay - self.nfft < 0:
            return
        xf = self._span(self.x, self.total - self.delay - self.nfft, self.nfft)
        if math.sqrt(float(np.mean(xf * xf))) < self.floor:
            return
        yf = self._span(self.y, self.total - self.nfft, self.nfft)
        X = np.fft.rfft(self.win * xf)
        Y = np.fft.rfft(self.win * yf)
        gxx, gyy, gxy = np.abs(X) ** 2, np.abs(Y) ** 2, np.conj(X) * Y
        if self.frames == 0:
            self.gxx, self.gyy, self.gxy = gxx, gyy, gxy
        else:
            self.gxx += self.alpha * (gxx - self.gxx)
            self.gyy += self.alpha * (gyy - self.gyy)
            self.gxy += self.alpha * (gxy - self.gxy)
        self.frames += 1

    # -- output -------------------------------------------------------------
    def result(self) -> LiveDelta:
        n = len(self.grid)
        delay_ms = None if self.delay is None else 1000.0 * self.delay / self.fs
        if self.frames == 0:
            return LiveDelta(self.grid, np.full(n, np.nan), np.full(n, np.nan), np.zeros(n, bool), delay_ms, 0)
        with np.errstate(invalid="ignore", divide="ignore"):
            h2 = np.where(self.gxx > 0, np.abs(self.gxy) ** 2 / self.gxx ** 2, 0.0)
            coh = np.where((self.gxx > 0) & (self.gyy > 0), np.abs(self.gxy) ** 2 / (self.gxx * self.gyy), 0.0)
        coh = np.clip(coh, 0.0, 1.0)
        power = smooth_power(self.freqs, h2, self.cfg.smoothing, self.grid, self.gxx)
        coherence = smooth_power(self.freqs, coh, self.cfg.smoothing, self.grid, self.gxx)
        measured = np.isfinite(power) & (power > 0)
        delta = np.where(measured, to_db(np.where(measured, power, 1.0)), np.nan)
        coherence = np.where(measured, coherence, np.nan)
        valid = measured & (coherence >= self.cfg.coherence_min)
        band = valid & (self.grid >= self.ref_band[0]) & (self.grid <= self.ref_band[1])
        ref = band if np.any(band) else valid
        if not np.any(ref):
            return LiveDelta(self.grid, np.full(n, np.nan), coherence, np.zeros(n, bool), delay_ms, self.frames)
        return LiveDelta(self.grid, delta - float(np.mean(delta[ref])), coherence, valid, delay_ms, self.frames)
