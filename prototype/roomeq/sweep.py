"""Exponential (log) sine sweep: generation, deconvolution, arrival detection.

Timeline of one capture (all indices in samples):

    recording: [ preroll | sweep ............ | tail ]
                         ^ sweep starts at index `preroll`

`deconvolve` returns an impulse response `h` together with `zero`, the
index of h that corresponds to recording sample 0. The linear IR of the
system therefore starts near ``zero + preroll + loop_delay``. Harmonic
distortion products land at *negative* times (before the linear IR),
which is what lets the log sweep reject them.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class SweepConfig:
    fs: int = 48000
    duration: float = 5.0          # seconds (spec: 2 / 5 / 10)
    f1: float = 20.0
    f2: float = 20000.0
    level_dbfs: float = -12.0      # sweep peak level
    fade_in_octaves: float = 1 / 6
    fade_out_octaves: float = 1 / 24
    preroll: float = 0.25          # seconds recorded before the sweep starts
    tail: float = 2.0              # seconds recorded after the sweep ends

    @property
    def n_sweep(self) -> int:
        return int(round(self.duration * self.fs))

    @property
    def n_preroll(self) -> int:
        return int(round(self.preroll * self.fs))

    @property
    def n_tail(self) -> int:
        return int(round(self.tail * self.fs))

    @property
    def n_recording(self) -> int:
        return self.n_preroll + self.n_sweep + self.n_tail

    @property
    def rate(self) -> float:
        """Sweep time constant L: f(t) = f1 * exp(t / L)."""
        return self.duration / np.log(self.f2 / self.f1)


def next_pow2(n: int) -> int:
    return 1 << int(np.ceil(np.log2(max(n, 1))))


def generate_sweep(cfg: SweepConfig) -> np.ndarray:
    """Farina exponential sine sweep from f1 to f2 with half-Hann fades.

    The fades are sized in octaves of sweep travel so they scale with
    sweep length. The regularised inverse divides by the actual sweep
    spectrum, so the fades cost a little SNR at the band edges but no
    accuracy.
    """
    n = cfg.n_sweep
    t = np.arange(n) / cfg.fs
    L = cfg.rate
    x = np.sin(2 * np.pi * cfg.f1 * L * (np.exp(t / L) - 1.0))

    n_in = max(1, int(round(L * np.log(2) * cfg.fade_in_octaves * cfg.fs)))
    n_out = max(1, int(round(L * np.log(2) * cfg.fade_out_octaves * cfg.fs)))
    x[:n_in] *= 0.5 * (1 - np.cos(np.pi * np.arange(n_in) / n_in))
    x[n - n_out:] *= 0.5 * (1 + np.cos(np.pi * np.arange(n_out) / n_out))

    return x * 10 ** (cfg.level_dbfs / 20)


def build_playback(cfg: SweepConfig, sweep: np.ndarray | None = None) -> np.ndarray:
    """What the plugin plays out: silence, sweep, silence (same length as the recording)."""
    if sweep is None:
        sweep = generate_sweep(cfg)
    out = np.zeros(cfg.n_recording)
    out[cfg.n_preroll:cfg.n_preroll + len(sweep)] = sweep
    return out


def _raised_cosine_log(f: np.ndarray, f_a: float, f_b: float) -> np.ndarray:
    """0 at f <= f_a, 1 at f >= f_b, raised-cosine in log frequency between."""
    with np.errstate(divide="ignore"):
        x = (np.log(np.maximum(f, 1e-9)) - np.log(f_a)) / (np.log(f_b) - np.log(f_a))
    x = np.clip(x, 0.0, 1.0)
    return 0.5 * (1 - np.cos(np.pi * x))


def inverse_spectrum(sweep: np.ndarray, nfft: int, fs: float, f1: float, f2: float,
                     reg_in_db: float = -50.0, transition_octaves: float = 1.0) -> np.ndarray:
    """Regularised inverse of the sweep: conj(X) / (|X|^2 + eps(f)).

    eps is tiny inside [f1, f2] (response is exact there) and large
    outside (the IR is band-limited), with a raised-cosine crossfade in
    log frequency over `transition_octaves` *outside* the band so the
    band itself stays untouched. The band-limit is zero-phase, so it rings
    slightly before the arrival; a gentle (1 octave) transition keeps that
    ringing short enough for the analysis window's pre-arrival part.
    """
    X = np.fft.rfft(sweep, nfft)
    f = np.fft.rfftfreq(nfft, 1 / fs)
    P = np.abs(X) ** 2
    in_band = (f >= f1) & (f <= f2)
    eps_in = np.median(P[in_band]) * 10 ** (reg_in_db / 10)
    eps_out = P[in_band].max()

    f_hi_edge = min(f2 * 2 ** transition_octaves, fs / 2)
    inside = _raised_cosine_log(f, f1 * 2 ** -transition_octaves, f1)
    if f_hi_edge > f2:
        inside = inside * (1 - _raised_cosine_log(f, f2, f_hi_edge))
    else:
        inside = inside * (f <= f2)
    # Interpolate eps in the log domain so the transition is smooth in dB.
    eps = np.exp(np.log(eps_out) + inside * (np.log(eps_in) - np.log(eps_out)))
    return np.conj(X) / (P + eps)


@dataclass
class Deconvolved:
    h: np.ndarray          # impulse response incl. negative times
    zero: int              # index in h of recording sample 0
    fs: float


def deconvolve(recording: np.ndarray, sweep: np.ndarray, fs: float,
               f1: float, f2: float) -> Deconvolved:
    """Linear deconvolution of a recording by the sweep that produced it."""
    n_rec, n_sw = len(recording), len(sweep)
    nfft = next_pow2(n_rec + n_sw)
    H = np.fft.rfft(recording, nfft) * inverse_spectrum(sweep, nfft, fs, f1, f2)
    h = np.fft.irfft(H, nfft)
    # Negative times wrapped to the end of the circular result; unwrap them.
    h = np.concatenate([h[nfft - n_sw:], h[:n_rec]])
    return Deconvolved(h=h, zero=n_sw, fs=fs)


def find_arrival(dec: Deconvolved, preroll: int, max_delay_s: float = 1.0) -> int:
    """Index in dec.h of the main arrival (peak of |h|) at or after sweep start.

    The search window bounds the loop delay (interface latency + acoustic
    path). Returns an absolute index into dec.h.
    """
    start = dec.zero + preroll
    stop = min(len(dec.h), start + int(max_delay_s * dec.fs))
    return start + int(np.argmax(np.abs(dec.h[start:stop])))


def fractional_peak_offset(a: np.ndarray, b: np.ndarray, max_lag: int = 64) -> float:
    """Delay of `b` relative to `a` in samples, with parabolic sub-sample refinement."""
    n = next_pow2(len(a) + len(b))
    xc = np.fft.irfft(np.conj(np.fft.rfft(a, n)) * np.fft.rfft(b, n), n)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    vals = np.concatenate([xc[:max_lag + 1], xc[n - max_lag:]])
    k = int(np.argmax(vals))
    lag = lags[k]
    y0 = xc[(lag - 1) % n]
    y1 = xc[lag % n]
    y2 = xc[(lag + 1) % n]
    denom = y0 - 2 * y1 + y2
    frac = 0.5 * (y0 - y2) / denom if denom != 0 else 0.0
    return float(lag + np.clip(frac, -0.5, 0.5))
