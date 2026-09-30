"""What the analysis found, in plain words.

A capture's measurement quality: signal-to-noise, coherence (music and pink
noise), repeatability (repeat sweeps) and usable range, each rated Excellent /
Good / Fair / Poor, and an overall confidence. The ratings are set by the worst
graded octave band, like the grade itself: Good and Fair start at the
grading's pass and marginal limits, so a PASS capture is Good or better.

- Signal-to-noise: Excellent from 30 dB, Good from 20, Fair from 10.
- Coherence (music and noise only; sweeps have none): the band's mean
  coherence over the bins the program excited. Excellent from 0.95, Good from
  0.85, Fair from 0.6.
- Repeatability (two or more sweeps): the worst 1/3-octave repeat spread
  beyond what the noise explains. Excellent up to 0.5 dB, Good up to 1, Fair
  up to 3.
- Usable range: where this capture is within 10 dB of its reference band
  (1-octave smoothed), as the session's is found; not found when the whole
  reference band is too noisy to use.
- Confidence: high; medium if anything is Fair or the grade is marginal; low
  if anything is Poor, the grade is redo or no band was heard.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .averaging import band_mask_weights, usable_range
from .capture import Capture
from .grading import Grade
from .spectrum import log_freq_grid, smooth_power, to_db

EXCELLENT, GOOD, FAIR, POOR = "Excellent", "Good", "Fair", "Poor"
HIGH, MEDIUM, LOW = "high", "medium", "low"

SNR_LIMITS_DB = (30.0, 20.0, 10.0)          # at or above: excellent, good, fair
COHERENCE_LIMITS = (0.95, 0.85, 0.6)        # at or above: excellent, good, fair
REPEAT_LIMITS_DB = (0.5, 1.0, 3.0)          # at or below: excellent, good, fair


def rate(value: float, limits: tuple[float, float, float], higher_is_better: bool = True) -> str:
    for label, limit in zip((EXCELLENT, GOOD, FAIR), limits):
        if (value >= limit) if higher_is_better else (value <= limit):
            return label
    return POOR


def _worse(a: str, b: str) -> str:
    order = (HIGH, MEDIUM, LOW)
    return a if order.index(a) >= order.index(b) else b


@dataclass
class MeasurementQuality:
    snr_db: float                        # worst graded band
    snr_band: str
    snr: str                             # its rating
    coherence: float | None              # worst graded band (music and noise only)
    coherence_band: str
    coherence_rating: str | None
    repeat_db: float | None              # worst excess repeat spread (two or more sweeps only)
    repeat_band: str
    repeatability: str | None
    usable: tuple[float, float]
    confidence: str = HIGH
    reasons: list[str] = field(default_factory=list)


def capture_usable_range(c: Capture, band: tuple[float, float]) -> tuple[float, float]:
    """Where the capture is within 10 dB of its reference band, 1-octave smoothed
    (redo bands left out). NaN when the whole reference band is redo."""
    grid = log_freq_grid(20.0, 20000.0, 48)
    level = to_db(smooth_power(c.freqs, c.power, 1, grid, band_mask_weights(c.freqs, c.grade) * c.weight))
    if not np.any(np.isfinite(level[(grid >= band[0]) & (grid <= band[1])])):
        return float("nan"), float("nan")
    return usable_range(grid, level, band)


def measurement_quality(c: Capture, band: tuple[float, float] = (250.0, 4000.0)) -> MeasurementQuality:
    graded = [b for b in c.grade.bands if not b.out_of_range]
    usable = capture_usable_range(c, band)
    if not graded:
        return MeasurementQuality(float("nan"), "", POOR, None, "", None, None, "", None, usable, LOW,
                                  ["no band was heard clearly"])

    worst = min(graded, key=lambda b: b.snr_db)
    q = MeasurementQuality(worst.snr_db, worst.name, rate(worst.snr_db, SNR_LIMITS_DB),
                           None, "", None, None, "", None, usable)
    with_coherence = [b for b in graded if b.coherence is not None]
    if with_coherence:
        b = min(with_coherence, key=lambda b: b.coherence)
        q.coherence, q.coherence_band, q.coherence_rating = b.coherence, b.name, rate(b.coherence, COHERENCE_LIMITS)
    with_repeats = [b for b in graded if b.excess_spread_db is not None]
    if with_repeats:
        b = max(with_repeats, key=lambda b: b.excess_spread_db)
        q.repeat_db, q.repeat_band = b.excess_spread_db, b.name
        q.repeatability = rate(b.excess_spread_db, REPEAT_LIMITS_DB, higher_is_better=False)

    for name, rating, detail in (("signal-to-noise", q.snr, f"{q.snr_db:.0f} dB at {q.snr_band} Hz"),
                                 ("coherence", q.coherence_rating,
                                  f"{q.coherence:.2f} at {q.coherence_band} Hz" if q.coherence is not None else ""),
                                 ("repeatability", q.repeatability,
                                  f"repeats {q.repeat_db:.1f} dB apart at {q.repeat_band} Hz" if q.repeat_db is not None else "")):
        if rating == POOR:
            q.confidence = _worse(q.confidence, LOW)
            q.reasons.append(f"{name} poor: {detail}")
        elif rating == FAIR:
            q.confidence = _worse(q.confidence, MEDIUM)
            q.reasons.append(f"{name} fair: {detail}")
    if c.grade.overall == Grade.REDO:
        q.confidence = _worse(q.confidence, LOW)
        q.reasons.append("graded redo")
    elif c.grade.overall == Grade.MARGINAL:
        q.confidence = _worse(q.confidence, MEDIUM)
        if not q.reasons:
            q.reasons.append("graded marginal")
    return q
