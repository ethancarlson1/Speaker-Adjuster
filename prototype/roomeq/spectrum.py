"""IR windowing, frequency grids, octave bands and fractional-octave smoothing."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .sweep import next_pow2


@dataclass(frozen=True)
class WindowConfig:
    """Analysis window around the main arrival (REW-style fixed window)."""
    pre: float = 0.050          # seconds kept before the arrival peak (holds band-limit pre-ringing)
    post: float = 0.500         # seconds kept after the arrival peak
    taper_pre: float = 0.5      # fraction of `pre` used for the rising half-Hann
    taper_post: float = 0.25    # fraction of `post` used for the falling half-Hann


def ir_window(cfg: WindowConfig, fs: float) -> tuple[np.ndarray, int]:
    """Window samples and the index of the arrival inside it."""
    n_pre = int(round(cfg.pre * fs))
    n_post = int(round(cfg.post * fs))
    w = np.ones(n_pre + n_post)
    n_a = max(1, int(round(n_pre * cfg.taper_pre)))
    n_b = max(1, int(round(n_post * cfg.taper_post)))
    w[:n_a] = 0.5 * (1 - np.cos(np.pi * np.arange(n_a) / n_a))
    w[len(w) - n_b:] = 0.5 * (1 + np.cos(np.pi * np.arange(n_b) / n_b))
    return w, n_pre


def response_nfft(cfg: WindowConfig, fs: float) -> int:
    n_pre = int(round(cfg.pre * fs))
    n_post = int(round(cfg.post * fs))
    return next_pow2(n_pre + n_post)


def log_freq_grid(f_lo: float = 20.0, f_hi: float = 20000.0, points_per_octave: int = 48) -> np.ndarray:
    n = int(np.floor(np.log2(f_hi / f_lo) * points_per_octave)) + 1
    return f_lo * 2.0 ** (np.arange(n) / points_per_octave)


def smooth_power(freqs: np.ndarray, power: np.ndarray, fraction: float | None,
                 out_freqs: np.ndarray, weights: np.ndarray | None = None) -> np.ndarray:
    """1/N-octave smoothing of a power spectrum onto `out_freqs`.

    Each output point is the (optionally weighted) mean power over
    [fc * 2^(-1/2N), fc * 2^(+1/2N)]. Bins are treated as piecewise-constant
    so windows narrower than a bin still behave. fraction=None means no
    smoothing (piecewise-constant lookup).

    `freqs` must be a uniform FFT grid starting at 0.
    """
    df = freqs[1] - freqs[0]
    w = np.ones_like(power) if weights is None else weights
    if fraction is None:
        idx = np.clip(np.round(out_freqs / df).astype(int), 0, len(power) - 1)
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(w[idx] > 0, power[idx], np.nan)

    edges = np.concatenate([[-0.5 * df], (np.arange(len(power)) + 0.5) * df])
    cum_wp = np.concatenate([[0.0], np.cumsum(w * power) * df])
    cum_w = np.concatenate([[0.0], np.cumsum(w) * df])

    half = 2.0 ** (1.0 / (2.0 * fraction))
    lo = out_freqs / half
    hi = out_freqs * half
    num = np.interp(hi, edges, cum_wp) - np.interp(lo, edges, cum_wp)
    den = np.interp(hi, edges, cum_w) - np.interp(lo, edges, cum_w)
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(den > 1e-12 * df, num / den, np.nan)


def rebin_power(freqs: np.ndarray, power: np.ndarray, out_freqs: np.ndarray) -> np.ndarray:
    """Box-average a power spectrum onto a coarser uniform FFT grid (both start at 0)."""
    df_in = freqs[1] - freqs[0]
    df_out = out_freqs[1] - out_freqs[0]
    edges = np.concatenate([[-0.5 * df_in], (np.arange(len(power)) + 0.5) * df_in])
    cum = np.concatenate([[0.0], np.cumsum(power) * df_in])
    lo = np.maximum(out_freqs - 0.5 * df_out, edges[0])
    hi = np.minimum(out_freqs + 0.5 * df_out, edges[-1])
    return (np.interp(hi, edges, cum) - np.interp(lo, edges, cum)) / np.maximum(hi - lo, 1e-12)


def to_db(power: np.ndarray, floor: float = 1e-20) -> np.ndarray:
    return 10 * np.log10(np.maximum(power, floor))


# Octave bands used for grading (IEC nominal centres 31.5 Hz ... 16 kHz).
OCTAVE_CENTERS = 1000.0 * 2.0 ** np.arange(-5, 5)
OCTAVE_NOMINAL = ["31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]


def octave_edges(center: float) -> tuple[float, float]:
    return center / np.sqrt(2), center * np.sqrt(2)


def format_hz(f: float) -> str:
    """Two significant figures: 88 Hz, 180 Hz, 1.4 kHz, 11 kHz."""
    rounded = float(f"{f:.2g}")
    if rounded >= 1000:
        k = rounded / 1000
        return f"{k:g} kHz"
    return f"{rounded:g} Hz"


def band_sum(freqs: np.ndarray, values: np.ndarray, lo: float, hi: float) -> tuple[float, int]:
    sel = (freqs >= lo) & (freqs < hi)
    return float(np.sum(values[sel])), int(np.count_nonzero(sel))
