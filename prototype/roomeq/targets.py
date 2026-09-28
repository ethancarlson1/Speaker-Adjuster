"""Target curves: flat, house, speech, and user-drawn ones.

A target is a list of (Hz, dB) points, interpolated in log frequency with a
shape-preserving cubic (PCHIP: no overshoot between points) and held flat
beyond the end points. Presets and user-drawn curves work the same way.

The curve is relative: it is placed so its 250 Hz-4 kHz mean matches the
measured average there (see `anchor_offset_db`), so the correction mostly
reshapes the ends rather than moving the overall level.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class TargetCurve:
    name: str
    points: tuple[tuple[float, float], ...]    # (Hz, dB), strictly increasing in Hz

    def db(self, freqs: np.ndarray) -> np.ndarray:
        return pchip_log(self.points, freqs)


FLAT = TargetCurve("Flat", ((1000.0, 0.0),))

# +4 dB below ~80 Hz easing to 0 by 250 Hz; flat mids; ~-1 dB/octave above 2 kHz (-3 dB at 16 kHz).
HOUSE = TargetCurve("House", (
    (20.0, 4.0), (50.0, 4.0), (80.0, 3.4), (125.0, 1.9), (180.0, 0.6), (250.0, 0.0),
    (2000.0, 0.0), (4000.0, -1.0), (8000.0, -2.0), (16000.0, -3.0), (20000.0, -3.3),
))

# Rolled off below 150 Hz (-6 dB at 75 Hz), +2 dB presence at 2-4 kHz, -3 dB at 16 kHz.
SPEECH = TargetCurve("Speech", (
    (20.0, -12.0), (35.0, -11.0), (50.0, -9.0), (75.0, -6.0), (100.0, -3.8), (150.0, -1.2), (250.0, 0.0),
    (1200.0, 0.0), (2000.0, 1.6), (2800.0, 2.0), (4000.0, 1.6), (6000.0, 0.0),
    (8000.0, -1.0), (16000.0, -3.0), (20000.0, -3.3),
))

PRESETS = (FLAT, HOUSE, SPEECH)


def pchip_log(points, freqs: np.ndarray) -> np.ndarray:
    """Fritsch-Carlson monotone cubic through `points` in log2(Hz)."""
    freqs = np.asarray(freqs, dtype=float)
    if len(points) == 1:
        return np.full(len(freqs), float(points[0][1]))
    x = np.log2([p[0] for p in points])
    y = np.array([p[1] for p in points], dtype=float)
    h = np.diff(x)
    delta = np.diff(y) / h
    n = len(x)
    m = np.zeros(n)
    m[0], m[-1] = delta[0], delta[-1]
    for k in range(1, n - 1):
        if delta[k - 1] * delta[k] > 0:
            w1 = 2 * h[k] + h[k - 1]
            w2 = h[k] + 2 * h[k - 1]
            m[k] = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k])
    if n > 2:      # keep the end slopes from overshooting (scipy's pchip end rule)
        for k, (d0, d1, hh0, hh1) in ((0, (delta[0], delta[1], h[0], h[1])),
                                     (n - 1, (delta[-1], delta[-2], h[-1], h[-2]))):
            s = ((2 * hh0 + hh1) * d0 - hh0 * d1) / (hh0 + hh1)
            if np.sign(s) != np.sign(d0):
                s = 0.0
            elif np.sign(d0) != np.sign(d1) and abs(s) > abs(3 * d0):
                s = 3 * d0
            m[k] = s

    xq = np.clip(np.log2(np.maximum(freqs, 1e-3)), x[0], x[-1])
    i = np.clip(np.searchsorted(x, xq, side="right") - 1, 0, n - 2)
    t = (xq - x[i]) / h[i]
    h00 = (1 + 2 * t) * (1 - t) ** 2
    h10 = t * (1 - t) ** 2
    h01 = t * t * (3 - 2 * t)
    h11 = t * t * (t - 1)
    return h00 * y[i] + h10 * h[i] * m[i] + h01 * y[i + 1] + h11 * h[i] * m[i + 1]


def anchor_offset_db(freqs_log: np.ndarray, level_db: np.ndarray, target: TargetCurve,
                     band: tuple[float, float] = (250.0, 4000.0)) -> float:
    """Level that places `target` on the measured average (mean difference over `band`)."""
    sel = (freqs_log >= band[0]) & (freqs_log <= band[1]) & np.isfinite(level_db)
    return float(np.mean(level_db[sel] - target.db(freqs_log[sel])))
