"""Multi-position power averaging, usable-range detection and quick mode."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .grading import CaptureGrade, Grade
from .spectrum import OCTAVE_CENTERS, log_freq_grid, smooth_power, to_db


def band_mask_weights(freqs: np.ndarray, grade: CaptureGrade) -> np.ndarray:
    """Per-bin weight in [0, 1]: 0 in bands graded REDO, 1 elsewhere.

    Interpolated in log frequency between octave centres, so a capture that
    is only bad below 88 Hz still contributes above it without a hard step.
    """
    band_w = np.array([0.0 if g == Grade.REDO else 1.0 for g in grade.band_grades()])
    logf = np.log2(np.maximum(freqs, 1e-3))
    return np.interp(logf, np.log2(OCTAVE_CENTERS), band_w)


def level_offset_db(freqs: np.ndarray, power: np.ndarray, band: tuple[float, float] = (250.0, 4000.0),
                    weights: np.ndarray | None = None) -> float:
    """Mean level (dB, 1/3-octave smoothed, log-spaced) over `band`."""
    grid = log_freq_grid(band[0], band[1], 24)
    return float(np.nanmean(to_db(smooth_power(freqs, power, 3, grid, weights))))


@dataclass
class AverageResult:
    freqs: np.ndarray
    power: np.ndarray          # weighted power average (linear FFT grid)
    weight: np.ndarray         # total weight per bin (0 = no usable capture)
    offsets_db: list[float]    # level alignment applied to each capture


def power_average(freqs: np.ndarray, powers: list[np.ndarray], weights: list[np.ndarray],
                  align_levels: bool = True, band: tuple[float, float] = (250.0, 4000.0)) -> AverageResult:
    """Weighted power average of |H|^2 across positions.

    With align_levels, each capture is first scaled so its mean level over
    `band` matches the mean across captures. That stops the closest
    position from dominating the average just because it is louder.
    """
    offsets = [0.0] * len(powers)
    if align_levels and powers:
        levels = [level_offset_db(freqs, p, band, w) for p, w in zip(powers, weights)]
        target = float(np.mean(levels))
        offsets = [target - lv for lv in levels]

    num = np.zeros_like(powers[0])
    den = np.zeros_like(powers[0])
    for p, w, off in zip(powers, weights, offsets):
        num += w * p * 10 ** (off / 10)
        den += w
    with np.errstate(invalid="ignore", divide="ignore"):
        avg = np.where(den > 0, num / np.maximum(den, 1e-30), np.nan)
    return AverageResult(freqs=freqs, power=avg, weight=den, offsets_db=offsets)


def usable_range(freqs_log: np.ndarray, level_db: np.ndarray, ref_band: tuple[float, float] = (250.0, 4000.0),
                 drop_db: float = 10.0, start_hz: float = 1000.0) -> tuple[float, float]:
    """PA usable range: walk down/up from `start_hz` until the response falls
    `drop_db` below the passband mean. Pass a heavily smoothed (1/1-octave)
    curve so single room modes don't end the walk early.
    """
    sel = (freqs_log >= ref_band[0]) & (freqs_log <= ref_band[1])
    ref = np.nanmean(level_db[sel])
    i0 = int(np.argmin(np.abs(freqs_log - start_hz)))
    lo, hi = freqs_log[0], freqs_log[-1]
    for i in range(i0, -1, -1):
        if not level_db[i] >= ref - drop_db:
            lo = freqs_log[min(i + 1, len(freqs_log) - 1)]
            break
    for i in range(i0, len(freqs_log)):
        if not level_db[i] >= ref - drop_db:
            hi = freqs_log[max(i - 1, 0)]
            break
    return float(lo), float(hi)


def target_level_db(freqs_log: np.ndarray, level_db: np.ndarray,
                    band: tuple[float, float] = (250.0, 4000.0)) -> float:
    """Level at which the (flat) target overlay is drawn."""
    sel = (freqs_log >= band[0]) & (freqs_log <= band[1])
    return float(np.nanmean(level_db[sel]))


@dataclass(frozen=True)
class QuickModePolicy:
    smoothing_fraction: int       # N of 1/N octave actually used for the average
    strength: float               # 0..1 scale on the correction (used in Phase 2)
    max_correction_db: float | None


def quick_mode_policy(n_good: int, user_fraction: int) -> QuickModePolicy:
    """Heavier smoothing and a capped, weaker correction until enough good
    positions exist. Strength reaches 100% at three good positions.
    """
    if n_good <= 1:
        return QuickModePolicy(min(user_fraction, 2), 0.5, 3.0)
    if n_good == 2:
        return QuickModePolicy(min(user_fraction, 3), 0.75, 6.0)
    return QuickModePolicy(user_fraction, 1.0, None)
