"""Minimum-phase EQ bands (RBJ cookbook biquads) and their responses.

Used by both the fitted correction and the voicing EQ. Bands are designed
at the plugin's sample rate, and every response here is the exact digital
one (bilinear warping included), so what the fit predicts is what plays.
"""

from __future__ import annotations

from dataclasses import dataclass, replace

import numpy as np

BELL = "bell"
LOW_SHELF = "low_shelf"
HIGH_SHELF = "high_shelf"
HIGH_PASS = "high_pass"
LOW_PASS = "low_pass"
KINDS = (BELL, LOW_SHELF, HIGH_SHELF, HIGH_PASS, LOW_PASS)

SHELF_Q = 1 / np.sqrt(2)       # RBJ shelf slope S = 1: steepest without overshoot


@dataclass(frozen=True)
class Band:
    kind: str
    freq: float                # Hz: centre (bell), half-gain point (shelf), -3 dB point (pass)
    gain_db: float = 0.0       # ignored by the pass filters
    q: float = SHELF_Q
    enabled: bool = True

    def scaled(self, amount: float) -> "Band":
        """Same band with its gain scaled (correction amount / quick-mode strength)."""
        return replace(self, gain_db=self.gain_db * amount)


def q_for_bandwidth(octaves: float) -> float:
    """Bell Q for a bandwidth in octaves (between the half-gain points, ignoring warping)."""
    return 1.0 / (2.0 * np.sinh(np.log(2) / 2 * octaves))


def bandwidth_for_q(q: float) -> float:
    return 2.0 / np.log(2) * np.arcsinh(1.0 / (2.0 * q))


def biquad(band: Band, fs: float) -> np.ndarray:
    """Normalised coefficients [b0, b1, b2, 1, a1, a2] (one scipy SOS row)."""
    if not band.enabled:
        return np.array([1.0, 0.0, 0.0, 1.0, 0.0, 0.0])
    f0 = min(band.freq, 0.49 * fs)
    w0 = 2 * np.pi * f0 / fs
    cw, sw = np.cos(w0), np.sin(w0)
    alpha = sw / (2 * band.q)
    a = 10 ** (band.gain_db / 40)
    if band.kind == BELL:
        b = [1 + alpha * a, -2 * cw, 1 - alpha * a]
        d = [1 + alpha / a, -2 * cw, 1 - alpha / a]
    elif band.kind == LOW_SHELF:
        k = 2 * np.sqrt(a) * alpha
        b = [a * ((a + 1) - (a - 1) * cw + k), 2 * a * ((a - 1) - (a + 1) * cw), a * ((a + 1) - (a - 1) * cw - k)]
        d = [(a + 1) + (a - 1) * cw + k, -2 * ((a - 1) + (a + 1) * cw), (a + 1) + (a - 1) * cw - k]
    elif band.kind == HIGH_SHELF:
        k = 2 * np.sqrt(a) * alpha
        b = [a * ((a + 1) + (a - 1) * cw + k), -2 * a * ((a - 1) + (a + 1) * cw), a * ((a + 1) + (a - 1) * cw - k)]
        d = [(a + 1) - (a - 1) * cw + k, 2 * ((a - 1) - (a + 1) * cw), (a + 1) - (a - 1) * cw - k]
    elif band.kind == HIGH_PASS:
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
        d = [1 + alpha, -2 * cw, 1 - alpha]
    elif band.kind == LOW_PASS:
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
        d = [1 + alpha, -2 * cw, 1 - alpha]
    else:
        raise ValueError(f"unknown band kind {band.kind!r}")
    return np.array([b[0] / d[0], b[1] / d[0], b[2] / d[0], 1.0, d[1] / d[0], d[2] / d[0]])


def sos(bands: list[Band], fs: float) -> np.ndarray:
    rows = [biquad(b, fs) for b in bands if b.enabled]
    return np.array(rows) if rows else np.array([[1.0, 0.0, 0.0, 1.0, 0.0, 0.0]])


def biquad_db(coeffs: np.ndarray, freqs: np.ndarray, fs: float) -> np.ndarray:
    """Exact magnitude (dB) of one biquad at `freqs`."""
    w = 2 * np.pi * np.asarray(freqs) / fs
    z1 = np.exp(-1j * w)
    z2 = z1 * z1
    num = coeffs[0] + coeffs[1] * z1 + coeffs[2] * z2
    den = coeffs[3] + coeffs[4] * z1 + coeffs[5] * z2
    return 20 * np.log10(np.maximum(np.abs(num), 1e-300) / np.maximum(np.abs(den), 1e-300))


def band_db(band: Band, freqs: np.ndarray, fs: float) -> np.ndarray:
    if not band.enabled:
        return np.zeros(len(freqs))
    return biquad_db(biquad(band, fs), freqs, fs)


def response_db(bands: list[Band], freqs: np.ndarray, fs: float) -> np.ndarray:
    """Combined magnitude (dB) of a chain of bands."""
    out = np.zeros(len(freqs))
    for b in bands:
        out += band_db(b, freqs, fs)
    return out
