"""Refit an EQ curve with fewer bands, for an output that has only N.

A console's output EQ often has 4-8 parametric bands; the correction can use
up to 10. Rather than drop the smallest, the curve the correction plays is
fitted again with at most N bands, by the correction's own band fit
(correction.fit_bands) with that curve as the wanted correction:

- the grid is 20 Hz-20 kHz at 24 points per octave, every point weighted the
  same (the curve is known everywhere, and flat outside the corrected range);
- never boost more than the curve does: the refit may exceed the curve's
  boost (or 0 dB where it cuts) by at most 0.5 dB, and cut at most 3 dB more;
- each band's gain stays within the curve's largest boost + 1 dB and its
  largest cut + 3 dB (both at most 15 dB); widths are the correction's;
- shelves are optional (consoles define a shelf's Q differently), and the
  widest bell is a setting (consoles go to Q 0.3, about 3.7 octaves).

Two fits, and the closer one is kept (by the fit's own cost):
- forward: the correction's greedy fit, adding up to N bands to a flat start;
- backward: start from the bands themselves (a shelf that isn't allowed
  becomes the widest bell an octave inside it), refine, then repeatedly drop
  the band whose removal costs least and refine the rest, until N are left.
  It keeps a narrow, deep cut that the forward fit would merge into a broad one.

Bands that already fit (no more than N, and no shelf where none is allowed)
are kept as they are.

The fit error compares the refit with the curve: the largest difference on
the grid, and the RMS over the points where either is beyond 0.1 dB.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import filters
from .correction import CorrectionConfig, FitProblem, _cost, _project, _Slot, fit_bands, levenberg_marquardt
from .filters import BELL, HIGH_SHELF, LOW_SHELF, Band
from .spectrum import log_freq_grid

POINTS_PER_OCTAVE = 24
BOOST_SLACK_DB = 0.5          # above the curve's boost (or 0 dB)
CUT_SLACK_DB = 3.0            # below the curve's cut (or 0 dB)
BOOST_HEADROOM_DB = 1.0       # a band's boost beyond the curve's largest
GAIN_LIMIT_DB = 15.0
ACTIVE_DB = 0.1               # RMS error over the points where either curve is beyond this


@dataclass
class Refit:
    bands: list[Band]
    refitted: bool             # False: the bands already fitted and are unchanged
    original_bands: int
    max_error_db: float        # refit vs curve, anywhere on the grid
    rms_error_db: float        # over the points where either is beyond ACTIVE_DB


def refit_grid() -> np.ndarray:
    return log_freq_grid(20.0, 20000.0, POINTS_PER_OCTAVE)


def fit_error(curve: np.ndarray, fitted: np.ndarray) -> tuple[float, float]:
    """(largest difference, RMS over the points where either curve is beyond ACTIVE_DB)."""
    diff = fitted - curve
    active = (np.abs(curve) > ACTIVE_DB) | (np.abs(fitted) > ACTIVE_DB)
    rms = float(np.sqrt(np.mean(diff[active] ** 2))) if np.any(active) else 0.0
    return float(np.max(np.abs(diff))) if len(diff) else 0.0, rms


def refit_bands(bands: list[Band], fs: float, max_bands: int, shelves: bool = True,
                max_octaves: float = CorrectionConfig.max_octaves) -> Refit:
    """At most `max_bands` bands that play `bands`' curve as closely as they can."""
    has_shelf = any(b.kind != BELL for b in bands)
    if len(bands) <= max_bands and (shelves or not has_shelf):
        return Refit(list(bands), False, len(bands), 0.0, 0.0)
    grid = refit_grid()
    curve = filters.response_db(bands, grid, fs)
    max_boost = min(GAIN_LIMIT_DB, max(0.0, float(np.max(curve))) + BOOST_HEADROOM_DB)
    max_cut = min(GAIN_LIMIT_DB, max(0.0, -float(np.min(curve))) + CUT_SLACK_DB)
    cfg = CorrectionConfig(max_bands=max_bands, max_cut_db=max_cut, max_boost_db=max_boost,
                           max_octaves=max_octaves, shelves=shelves, outside_weight=1.0,
                           min_gain_db=0.1, min_improvement=0.005, stop_rms_db=0.02)
    prob = FitProblem(freqs=grid, fs=fs, desired=curve, weight=np.ones(len(grid)),
                      upper=np.maximum(curve, 0.0) + BOOST_SLACK_DB,
                      lower=np.minimum(curve, 0.0) - CUT_SLACK_DB,
                      f_lo=20.0, f_hi=20000.0, max_cut=max_cut, max_boost=max_boost, cfg=cfg)
    forward = fit_bands(prob) if max_bands > 0 else []
    backward = _prune(bands, prob, max_bands)
    zero = np.zeros(len(grid))
    cost_f = _cost(prob, filters.response_db(forward, grid, fs) + zero)
    cost_b = _cost(prob, filters.response_db(backward, grid, fs) + zero)
    fitted = backward if cost_b < cost_f else forward
    max_err, rms = fit_error(curve, filters.response_db(fitted, grid, fs))
    return Refit(fitted, True, len(bands), max_err, rms)


def _slot(b: Band, prob: FitProblem) -> _Slot:
    """A band as a fit slot, within the refit's bounds (a shelf that isn't allowed: the widest bell)."""
    sign = 1.0 if b.gain_db > 0 else -1.0
    kind, freq = b.kind, b.freq
    if kind != BELL and not prob.cfg.shelves:
        kind, freq = BELL, b.freq / 2 if b.kind == LOW_SHELF else b.freq * 2
    if kind == BELL:
        q = b.q if b.kind == BELL else filters.q_for_bandwidth(prob.cfg.max_octaves)
        p = np.array([np.log2(freq), b.gain_db, np.log2(q)])
    else:
        p = np.array([np.log2(freq), b.gain_db])
    return _Slot(kind, sign, _project(kind, p, sign, prob))


def _prune(bands: list[Band], prob: FitProblem, n: int) -> list[Band]:
    """Backward: refine the bands, then drop the least useful and refine, until n are left."""
    zero = np.zeros(len(prob.freqs))
    slots = [_slot(b, prob) for b in bands if b.enabled and b.kind in (BELL, LOW_SHELF, HIGH_SHELF)]
    if slots:
        levenberg_marquardt(slots, list(range(len(slots))), prob, zero)
    while len(slots) > n:
        responses = [filters.band_db(s.band(), prob.freqs, prob.fs) for s in slots]
        total = sum(responses, zero)
        costs = [_cost(prob, total - r) for r in responses]
        del slots[int(np.argmin(costs))]
        if slots:
            levenberg_marquardt(slots, list(range(len(slots))), prob, zero)
    return sorted((s.band() for s in slots if abs(s.p[1]) >= prob.cfg.min_gain_db), key=lambda b: b.freq)
