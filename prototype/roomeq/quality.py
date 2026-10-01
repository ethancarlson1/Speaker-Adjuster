"""What the analysis found, in plain words.

Two parts: each capture's measurement quality (below), and the system summary
at the Correct step (further down), with the reasons the correction was held
back wherever it was.

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

import warnings
from dataclasses import dataclass, field

import numpy as np

from .averaging import SessionSummary, band_mask_weights, usable_range
from .capture import Capture
from .correction import CorrectionConfig, CorrectionResult, _aligned_positions
from .grading import Grade
from .spectrum import format_hz, log_freq_grid, smooth_power, to_db

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


# ---------------------------------------------------------------------------
# System summary (the Correct step)
#
# - Measurement confidence: high when every capture in the average is high
#   confidence and there are three or more good positions; medium when any
#   is medium or low, or there are fewer; low with no good position, or when
#   more than half of them are low.
# - Coverage consistency: how much the positions differ, as the mean
#   (over the fit range) of their standard deviation at each frequency,
#   1/3-octave smoothed and level-aligned: "+-2.8 dB". Excellent up to 1.5 dB,
#   Good up to 3, Fair up to 5.
# - Largest broad issue: the biggest 1/3-octave difference between the
#   average and the target in the fit range, outside nulls.
# - The proposal: its filters, its largest cut and boost (the combined
#   curve), the nulls it declined to boost (where the target asked for a
#   boost), and the RMS difference from the target before and after (fit
#   range, outside nulls).
# - Why it held back: every place the fit was limited, from the state it
#   worked from (see explain()).

COVERAGE_LIMITS_DB = (1.5, 3.0, 5.0)     # at or below: excellent, good, fair


@dataclass
class Explanation:
    kind: str            # positions, bandwidth, null, disagreement, limit, noise, coherence, repeatability
    lo_hz: float         # where (0, 0: everywhere)
    hi_hz: float
    text: str


@dataclass
class SystemSummary:
    confidence: str
    confidence_reasons: list[str]
    positions: int                       # good positions in the average
    variation_db: float | None           # position-to-position, +- dB (None with one position)
    coverage: str | None                 # its rating
    usable: tuple[float, float]
    issue_db: float | None               # largest broad difference from the target (+: above it)
    issue_hz: float | None
    filters: int
    largest_cut_db: float                # of the combined correction (0: none)
    largest_boost_db: float
    nulls_ignored: int
    before_db: float                     # RMS difference from the target, fit range, outside nulls
    after_db: float
    explanations: list[Explanation] = field(default_factory=list)


def _runs(mask: np.ndarray) -> list[tuple[int, int]]:
    """Contiguous True runs as (first, last) index pairs."""
    runs, start = [], None
    for i, m in enumerate(mask):
        if m and start is None:
            start = i
        elif not m and start is not None:
            runs.append((start, i - 1))
            start = None
    if start is not None:
        runs.append((start, len(mask) - 1))
    return runs


def _names(names: list[str]) -> str:
    return names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]


def explain(captures, summary: SessionSummary, result: CorrectionResult, cfg: CorrectionConfig) -> list[Explanation]:
    """Why the correction was held back, wherever it was: few positions, the
    PA's range, nulls (dips and disagreement), the cut and boost limits, and
    bands a position was too noisy in."""
    out = []
    grid = result.grid
    lo, hi = result.fit_range
    inside = (grid >= lo) & (grid <= hi)
    finite = np.isfinite(result.average_db)
    want = result.target_db - result.average_db          # the correction each point asks for, before limits
    policy = summary.policy

    if policy.strength < 1.0:
        n = summary.n_good
        start = "No position graded better than redo" if n == 0 else f"Only {n} good position{'s' if n != 1 else ''}"
        out.append(Explanation("positions", 0.0, 0.0,
                               f"{start}: the fit smooths to 1/{policy.smoothing_fraction} octave and is held to "
                               f"{policy.strength:.0%} strength, at most {policy.max_correction_db:g} dB. "
                               f"Measure {3 - n} more to correct fully."))

    pa_lo, pa_hi = result.pa_range     # (the grid's ends when the PA never drops that far)
    if lo == pa_lo and pa_lo > cfg.range_hz[0] and pa_lo > grid[0]:
        out.append(Explanation("bandwidth", cfg.range_hz[0], lo,
                               f"Not correcting below {format_hz(lo)}: the system is more than {cfg.rolloff_db:g} dB down "
                               "there (its low-frequency limit), and boosting it costs headroom for little gain."))
    if hi == pa_hi and pa_hi < cfg.range_hz[1] and pa_hi < grid[-1]:
        out.append(Explanation("bandwidth", hi, cfg.range_hz[1],
                               f"Not correcting above {format_hz(hi)}: the system is more than {cfg.rolloff_db:g} dB down "
                               "there (its high-frequency limit), and boosting it costs headroom for little gain."))

    # Nulls that stopped a boost the target asked for.
    for a, b in _runs(result.null_mask & inside):
        span = slice(a, b + 1)
        wanted = np.where(finite[span], want[span], -np.inf)
        if not np.max(wanted) > cfg.min_gain_db:
            continue
        f = float(grid[a + int(np.argmax(wanted))])
        dip = result.dip_db[span]
        spread = result.spread_db[span]
        is_dip = np.any(dip < -cfg.null_dip_db)
        worst_spread = float(np.nanmax(spread)) if np.any(np.isfinite(spread)) else float("nan")
        disagree = np.isfinite(worst_spread) and worst_spread > cfg.null_spread_db
        if is_dip:
            text = (f"Not boosting the dip at {format_hz(f)} ({-float(np.nanmin(dip)):.0f} dB below the trend): it looks like "
                    "a cancellation that moves from seat to seat, and boosting it wastes power.")
            if disagree:
                text += f" The positions also disagree there, by up to {worst_spread:.0f} dB."
            out.append(Explanation("null", float(grid[a]), float(grid[b]), text))
        elif disagree:
            out.append(Explanation("disagreement", float(grid[a]), float(grid[b]),
                                   f"Not boosting around {format_hz(f)}: the positions disagree by up to {worst_spread:.0f} dB "
                                   "there, so no single correction suits them all."))

    # Where the target asked for more than the limits allow.
    capped = policy.max_correction_db is not None
    for sign in (1.0, -1.0):
        limit = result.max_boost_db if sign > 0 else result.max_cut_db
        over = inside & finite & ~(result.null_mask if sign > 0 else np.zeros(len(grid), bool)) & (sign * want > limit + 0.5)
        for a, b in _runs(over):
            span = slice(a, b + 1)
            i = a + int(np.argmax(sign * want[span]))
            applied = limit * result.strength
            user = cfg.max_boost_db if sign > 0 else cfg.max_cut_db
            source = "the quick-mode cap" if capped and applied < user - 1e-9 else ("Max boost" if sign > 0 else "Max cut")
            if sign > 0:
                text = (f"Boost held to +{applied:.1f} dB at {format_hz(grid[i])} ({source}): the response is "
                        f"{want[i]:.1f} dB below the target there.")
            else:
                text = (f"Cut held to -{applied:.1f} dB at {format_hz(grid[i])} ({source}): the response is "
                        f"{-want[i]:.1f} dB above the target there.")
            out.append(Explanation("limit", float(grid[a]), float(grid[b]), text))

    # Bands a position was left out of (graded redo), in the fit range.
    included = [c for c in captures if not c.excluded]
    by_band: dict[str, list[tuple[str, str]]] = {}
    order = []
    for c in included:
        for band in c.grade.bands:
            if band.out_of_range or band.grade != Grade.REDO or not (band.hi > lo and band.lo < hi):
                continue
            if band.snr_grade == Grade.REDO:
                cause = "coherence" if c.kind != "sweep" else "noise"
            else:
                cause = "repeatability"
            if band.name not in by_band:
                by_band[band.name] = []
                order.append((band.name, band.lo, band.hi))
            by_band[band.name].append((c.name, cause))
    words = {"noise": "too noisy", "coherence": "low coherence", "repeatability": "the repeats disagreed"}
    for name, blo, bhi in order:
        entries = by_band[name]
        causes = []
        for _, cause in entries:
            if cause not in causes:
                causes.append(cause)
        why = "; ".join(words[c] for c in causes)
        kind = causes[0] if len(causes) == 1 else "noise"
        if len(entries) == len(included):
            text = f"{name} Hz band: no position was clean enough there ({why}), so it isn't corrected."
        else:
            names = _names([n for n, _ in entries])
            text = f"{name} Hz band: {names} left out of the average there ({why})."
        out.append(Explanation(kind, float(blo), float(bhi), text))
    return out


def system_summary(captures, summary: SessionSummary, result: CorrectionResult,
                   cfg: CorrectionConfig = CorrectionConfig()) -> SystemSummary:
    grid = result.grid
    lo, hi = result.fit_range
    inside = (grid >= lo) & (grid <= hi)
    finite = np.isfinite(result.average_db)
    judge = inside & ~result.null_mask & finite

    # Confidence, from the captures in the average.
    included = [c for c in captures if not c.excluded]
    qualities = [(c.name, measurement_quality(c, cfg.ref_band)) for c in included]
    confidence, reasons = HIGH, []
    for name, q in qualities:
        if q.confidence != HIGH:
            confidence = _worse(confidence, MEDIUM)
            reasons.append(f"{name}: {q.confidence}" + (f" ({q.reasons[0]})" if q.reasons else ""))
    n_low = sum(1 for _, q in qualities if q.confidence == LOW)
    if summary.n_good == 0 or n_low * 2 > len(qualities):
        confidence = LOW
    if summary.n_good < 3:
        confidence = _worse(confidence, MEDIUM)
        reasons.insert(0, f"{summary.n_good} good position{'s' if summary.n_good != 1 else ''} (3 or more for full confidence)")

    # How much the positions differ.
    positions = _aligned_positions(captures, summary, 3, grid)
    variation = coverage = None
    if len(positions) >= 2:
        stack = np.array(positions)[:, inside]
        valid = np.sum(np.isfinite(stack), axis=0) >= 2
        if np.any(valid):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", RuntimeWarning)
                sd = np.nanstd(stack[:, valid], axis=0)
            variation = float(np.mean(sd))
            coverage = rate(variation, COVERAGE_LIMITS_DB, higher_is_better=False)

    # The largest broad issue.
    broad = to_db(smooth_power(summary.freqs, summary.power, 3, grid, summary.weight)) - result.target_db
    issue_db = issue_hz = None
    ok = judge & np.isfinite(broad)
    if np.any(ok):
        i = int(np.flatnonzero(ok)[np.argmax(np.abs(broad[ok]))])
        issue_db, issue_hz = float(broad[i]), float(grid[i])

    err = result.average_db - result.target_db
    before = float(np.sqrt(np.mean(err[judge] ** 2))) if np.any(judge) else float("nan")
    explanations = explain(captures, summary, result, cfg)
    return SystemSummary(
        confidence=confidence, confidence_reasons=reasons, positions=summary.n_good, variation_db=variation,
        coverage=coverage, usable=summary.usable, issue_db=issue_db, issue_hz=issue_hz, filters=len(result.bands),
        largest_cut_db=float(min(np.min(result.correction_db), 0.0)),
        largest_boost_db=float(max(np.max(result.correction_db), 0.0)),
        nulls_ignored=sum(1 for e in explanations if e.kind in ("null", "disagreement")), before_db=before,
        after_db=result.rms_error_db, explanations=explanations)
