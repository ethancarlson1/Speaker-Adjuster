"""Pink noise measurement signal.

Played on one speaker and analysed with the dual-FFT (dualfft.py) against
the known noise, graded like any program capture. Compared with the sweep it
needs longer (15-30 s) and doesn't separate out distortion, but it averages
away a stray bang and fills every frequency continuously.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .sweep import next_pow2


@dataclass(frozen=True)
class NoiseConfig:
    fs: int = 48000
    duration: float = 20.0         # seconds (10 / 20 / 30)
    f1: float = 20.0
    f2: float = 20000.0
    level_dbfs: float = -12.0      # peak level, like the sweep
    fade: float = 0.05             # seconds, raised-cosine in and out
    tail: float = 1.0              # seconds recorded after the noise stops
    seed: int = 1

    @property
    def n_noise(self) -> int:
        return int(round(self.duration * self.fs))

    @property
    def n_tail(self) -> int:
        return int(round(self.tail * self.fs))


def generate_pink_noise(cfg: NoiseConfig) -> np.ndarray:
    """Band-limited pink noise (-3 dB/octave, f1..f2), peak-normalised to level_dbfs.

    The level is a peak level, the same as the sweep's, so the speaker never
    sees more than that. Peaks are clipped at 3 sigma (crest factor ~9.5 dB),
    so the noise sits ~6.5 dB below a sweep's RMS at the same setting.
    """
    n = cfg.n_noise
    nfft = next_pow2(n)
    rng = np.random.default_rng(cfg.seed)
    spec = np.fft.rfft(rng.standard_normal(nfft))
    f = np.fft.rfftfreq(nfft, 1 / cfg.fs)
    shape = np.where((f >= cfg.f1) & (f <= cfg.f2), 1 / np.sqrt(np.maximum(f, cfg.f1)), 0.0)
    x = np.fft.irfft(spec * shape, nfft)[:n]

    # Clip the rare peaks at 3 sigma: crest factor ~14 dB -> ~9.5 dB, i.e. ~4.5 dB
    # more signal for the same peak level. The analysis compares against exactly
    # what was played, so the clipping costs no accuracy.
    x /= np.sqrt(np.mean(x ** 2))
    np.clip(x, -3.0, 3.0, out=x)

    n_fade = max(1, int(round(cfg.fade * cfg.fs)))
    ramp = 0.5 * (1 - np.cos(np.pi * np.arange(n_fade) / n_fade))
    x[:n_fade] *= ramp
    x[n - n_fade:] *= ramp[::-1]
    return x * (10 ** (cfg.level_dbfs / 20) / np.max(np.abs(x)))


def build_noise_playback(cfg: NoiseConfig, noise: np.ndarray | None = None) -> np.ndarray:
    """What the plugin plays: the noise, then `tail` of silence while the room decays."""
    if noise is None:
        noise = generate_pink_noise(cfg)
    return np.concatenate([noise, np.zeros(cfg.n_tail)])
