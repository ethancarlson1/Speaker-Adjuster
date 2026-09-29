"""Offline room simulator: PA model -> pyroomacoustics room -> mic, plus load-in noise.

Levels are digital (dBFS at the mic input). pyroomacoustics RIRs carry 1/r
spreading with unity direct sound at 1 m, which gives realistic relative
levels between near and far positions (about -20 dBFS RMS at 5 m for a
-12 dBFS sweep).
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache

import numpy as np
import scipy.signal as ss

CENTER_FREQS = (125, 250, 500, 1000, 2000, 4000)


@dataclass(frozen=True)
class RoomSpec:
    """Default: empty club, 12 x 18 x 5 m, T30 about 0.9 s at 500 Hz, 1.4 s at 125 Hz."""
    dims: tuple[float, float, float] = (12.0, 18.0, 5.0)      # width, depth, height (m)
    source: tuple[float, float, float] = (6.0, 1.0, 2.5)      # PA, front centre
    walls: tuple[float, ...] = (0.25, 0.30, 0.35, 0.40, 0.40, 0.40)
    floor: tuple[float, ...] = (0.05, 0.05, 0.06, 0.07, 0.08, 0.08)
    ceiling: tuple[float, ...] = (0.45, 0.55, 0.60, 0.65, 0.65, 0.65)
    max_order: int = 3
    n_rays: int = 20000
    seed: int = 1               # ray tracing is random; seeded so runs are repeatable


# Audience-area positions at varied distance and off-axis, none on the centre line (x = 6).
DEFAULT_POSITIONS: tuple[tuple[float, float, float], ...] = (
    (4.5, 6.0, 1.6),
    (8.2, 9.0, 1.6),
    (3.5, 12.5, 1.6),
    (7.5, 15.0, 1.6),
    (9.5, 5.0, 1.6),
)


@dataclass(frozen=True)
class PASpec:
    """A plausible small PA: 55 Hz high-pass, a low-mid bump, a crossover dip, a horn peak."""
    hp_hz: float = 55.0
    hp_order: int = 4
    lp_hz: float = 18000.0
    lp_order: int = 2
    peq: tuple[tuple[float, float, float], ...] = ((150.0, 3.0, 0.9), (2800.0, -4.0, 1.8), (6000.0, 2.5, 1.5))
    h2_db: float = -40.0        # 2nd / 3rd harmonic at `ref_level_dbfs` drive
    h3_db: float = -45.0
    ref_level_dbfs: float = -12.0
    latency_ms: float = 6.0     # interface round trip + PA DSP


@dataclass(frozen=True)
class NoiseSpec:
    """Noise at the mic, in dBFS RMS. None disables a component."""
    pink_dbfs: float | None = -55.0        # broadband background
    rumble_dbfs: float | None = None       # low rumble, 25 Hz to `rumble_hz` (HVAC, truck idling)
    rumble_hz: float = 100.0
    babble_dbfs: float | None = None       # crowd chatter (speech-shaped, modulated)
    bursts: int = 0                        # impulsive load-in noises (drops, hammer hits)
    burst_dbfs: float = -20.0              # RMS over each burst's first 20 ms
    burst_times: tuple[float, ...] | None = None   # seconds; random if None


def _peaking_sos(f0: float, gain_db: float, q: float, fs: float) -> np.ndarray:
    """RBJ cookbook peaking EQ as one SOS section."""
    a = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / fs
    alpha = np.sin(w0) / (2 * q)
    b = np.array([1 + alpha * a, -2 * np.cos(w0), 1 - alpha * a])
    den = np.array([1 + alpha / a, -2 * np.cos(w0), 1 - alpha / a])
    return np.concatenate([b / den[0], den / den[0]])[None, :]


def pa_sos(pa: PASpec, fs: float) -> np.ndarray:
    sections = [ss.butter(pa.hp_order, pa.hp_hz, "highpass", fs=fs, output="sos"),
                ss.butter(pa.lp_order, min(pa.lp_hz, 0.45 * fs), "lowpass", fs=fs, output="sos")]
    sections += [_peaking_sos(f, g, q, fs) for f, g, q in pa.peq]
    return np.vstack(sections)


@lru_cache(maxsize=8)
def _room_rirs(room: RoomSpec, positions: tuple[tuple[float, float, float], ...], fs: int) -> tuple[np.ndarray, ...]:
    import pyroomacoustics as pra

    def mat(coeffs):
        return pra.Material({"coeffs": list(coeffs), "center_freqs": list(CENTER_FREQS)})

    materials = {w: mat(room.walls) for w in ("east", "west", "north", "south")}
    materials["floor"] = mat(room.floor)
    materials["ceiling"] = mat(room.ceiling)
    sim = pra.ShoeBox(list(room.dims), fs=fs, materials=materials, max_order=room.max_order,
                      air_absorption=True, ray_tracing=True)
    sim.set_ray_tracing(receiver_radius=0.5, n_rays=room.n_rays, energy_thres=1e-7)
    sim.add_source(list(room.source))
    sim.add_microphone_array(np.array(positions, dtype=float).T)
    pra.random.seed(room.seed)
    sim.compute_rir()
    return tuple(np.asarray(sim.rir[m][0]) for m in range(len(positions)))


def pink_noise(n: int, fs: float, rng: np.random.Generator, f_lo: float = 20.0, f_hi: float = 20000.0) -> np.ndarray:
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1 / fs)
    shape = np.where((f >= f_lo) & (f <= f_hi), 1 / np.sqrt(np.maximum(f, f_lo)), 0.0)
    x = np.fft.irfft(spec * shape, n)
    return x / np.sqrt(np.mean(x ** 2))


def make_noise(n: int, fs: float, spec: NoiseSpec, rng: np.random.Generator) -> np.ndarray:
    out = np.zeros(n)
    if spec.pink_dbfs is not None:
        out += pink_noise(n, fs, rng) * 10 ** (spec.pink_dbfs / 20)
    if spec.rumble_dbfs is not None:
        r = ss.sosfilt(ss.butter(4, [25.0, spec.rumble_hz], "bandpass", fs=fs, output="sos"),
                       pink_noise(n, fs, rng))
        out += r / np.sqrt(np.mean(r ** 2)) * 10 ** (spec.rumble_dbfs / 20)
    if spec.babble_dbfs is not None:
        b = ss.sosfilt(ss.butter(2, [150, 4000], "bandpass", fs=fs, output="sos"), pink_noise(n, fs, rng))
        env = 1 + 0.8 * np.sin(2 * np.pi * 4.0 * np.arange(n) / fs + rng.uniform(0, 2 * np.pi))
        env *= 1 + 0.5 * pink_noise(n, fs, rng, 0.5, 8.0)
        b *= np.maximum(env, 0)
        out += b / np.sqrt(np.mean(b ** 2)) * 10 ** (spec.babble_dbfs / 20)
    times = spec.burst_times if spec.burst_times is not None else rng.uniform(0, n / fs, spec.bursts)
    for t0 in times:
        i = int(t0 * fs)
        m = min(n - i, int(0.3 * fs))
        if m <= 0:
            continue
        tt = np.arange(m) / fs
        burst = rng.standard_normal(m) * np.exp(-tt / 0.04)
        burst = ss.sosfilt(ss.butter(2, rng.uniform(2000, 8000), "lowpass", fs=fs, output="sos"), burst)
        head = burst[: int(0.02 * fs)]
        burst *= 10 ** (spec.burst_dbfs / 20) / np.sqrt(np.mean(head ** 2))
        out[i:i + m] += burst
    return out


class SimulatedRoom:
    def __init__(self, room: RoomSpec = RoomSpec(), pa: PASpec = PASpec(),
                 positions: tuple[tuple[float, float, float], ...] = DEFAULT_POSITIONS, fs: int = 48000):
        self.room, self.pa, self.positions, self.fs = room, pa, positions, fs
        self.rirs = _room_rirs(room, positions, fs)
        self.sos = pa_sos(pa, fs)
        self.latency = int(round(pa.latency_ms * fs / 1000))
        self.amp = 10 ** (pa.ref_level_dbfs / 20)
        # For x = A sin: a2*x^2 puts a2*A/2 at 2f; a3*(x^3 - 3/4 A^2 x) puts
        # a3*A^2/4 at 3f without changing the fundamental.
        self.a2 = 2 * 10 ** (pa.h2_db / 20) / self.amp
        self.a3 = 4 * 10 ** (pa.h3_db / 20) / self.amp ** 2

    def distance(self, pos: int) -> float:
        return float(np.linalg.norm(np.subtract(self.positions[pos], self.room.source)))

    def linear_ir(self, pos: int, length_s: float = 3.0) -> np.ndarray:
        """Ground truth: PA (linear part) * room, including the loop latency."""
        n = int(length_s * self.fs)
        imp = np.zeros(n)
        imp[self.latency] = 1.0
        return ss.fftconvolve(ss.sosfilt(self.sos, imp), self.rirs[pos])[:n]

    def distort(self, x: np.ndarray) -> np.ndarray:
        """Driver nonlinearity, applied at 4x oversampling so harmonics above
        Nyquist are filtered (as an ADC would) instead of aliasing back.
        Applied before the PA's filters so its high-pass removes the DC
        that x^2 produces (air can't carry DC to the mic)."""
        if self.a2 == 0 and self.a3 == 0:
            return x
        up = ss.resample_poly(x, 4, 1)
        up = up + self.a2 * up ** 2 + self.a3 * (up ** 3 - 0.75 * self.amp ** 2 * up)
        return ss.resample_poly(up, 1, 4)[:len(x)]

    def play(self, x: np.ndarray, pos: int, noise: NoiseSpec = NoiseSpec(),
             rng: np.random.Generator | None = None, drift_ppm: float = 0.0) -> np.ndarray:
        """What the measurement mic records while the plugin outputs `x`.

        drift_ppm: the mic's clock runs this much fast (+) or slow (-) against
        the output's, as with two interfaces or an aggregate device.
        """
        rng = rng if rng is not None else np.random.default_rng()
        y = ss.sosfilt(self.sos, self.distort(x))
        y = ss.fftconvolve(y, self.rirs[pos])[:len(x)]
        y = np.concatenate([np.zeros(self.latency), y])[:len(x)]
        if drift_ppm:
            # Stretch a zero-padded copy whose length makes the drift a whole number
            # of samples (exact for whole-number ppm), band-limited (FFT resampling).
            L = -(-len(y) // 1_000_000) * 1_000_000
            padded = np.concatenate([y, np.zeros(L - len(y))])
            y = ss.resample(padded, L + int(round(L * drift_ppm * 1e-6)))[:len(x)]
        return y + make_noise(len(x), self.fs, noise, rng)


def synthetic_program(duration: float, fs: int, rng: np.random.Generator, level_dbfs: float = -18.0) -> np.ndarray:
    """Band-like walk-in music: kick, snare, hats, bass, chord pad, lead line.

    Uneven spectrum and gaps on purpose: that's what makes program-material
    measurement harder than a sweep.
    """
    n = int(duration * fs)
    out = np.zeros(n)
    beat = 0.5  # 120 bpm

    def add(i, sig):
        m = min(len(sig), n - i)
        if m > 0:
            out[i:i + m] += sig[:m]

    def tone(f0, dur, harmonics, rolloff, decay):
        tt = np.arange(int(dur * fs)) / fs
        ks = np.arange(1, harmonics + 1)
        ks = ks[ks * f0 < 0.45 * fs]
        s = np.sum(np.sin(2 * np.pi * np.outer(ks, tt) * f0 + rng.uniform(0, 2 * np.pi, (len(ks), 1)))
                   / ks[:, None] ** rolloff, axis=0)
        return s * np.exp(-tt / decay) * np.minimum(1, tt / 0.005)

    roots = [41.2, 65.4, 49.0, 73.4]                       # E1 C2 G1 D2
    chords = [(164.8, 196.0, 246.9), (130.8, 164.8, 196.0), (196.0, 246.9, 293.7), (146.8, 185.0, 220.0)]
    lead_notes = [329.6, 392.0, 440.0, 493.9, 587.3, 659.3, 784.0, 880.0, 987.8]

    for b, tb in enumerate(np.arange(0, duration, beat)):
        i = int(tb * fs)
        bar = (b // 4) % 4
        # kick
        tt = np.arange(int(0.35 * fs)) / fs
        f = 45 + 60 * np.exp(-tt / 0.03)
        add(i, 0.9 * np.sin(2 * np.pi * np.cumsum(f) / fs) * np.exp(-tt / 0.12))
        # snare on 2 and 4
        if b % 2 == 1:
            m = int(0.25 * fs)
            sn = ss.sosfilt(ss.butter(2, [180, 7000], "bandpass", fs=fs, output="sos"), rng.standard_normal(m))
            add(i, 0.35 * sn * np.exp(-np.arange(m) / fs / 0.06))
        # hats on eighths
        for off in (0.0, beat / 2):
            m = int(0.06 * fs)
            hh = ss.sosfilt(ss.butter(2, 7000, "highpass", fs=fs, output="sos"), rng.standard_normal(m))
            add(i + int(off * fs), 0.12 * hh * np.exp(-np.arange(m) / fs / 0.015))
        # bass follows the root, lead picks a note per beat
        add(i, 0.5 * tone(roots[bar], beat, 30, 1.0, 0.4))
        add(i, 0.12 * tone(rng.choice(lead_notes), beat * 0.9, 12, 1.3, 0.5))
        if b % 4 == 0:
            for f0 in chords[bar]:
                add(i, 0.10 * tone(f0, 4 * beat, 16, 1.6, 1.5))

    out *= 10 ** (level_dbfs / 20) / np.sqrt(np.mean(out ** 2))
    return out
