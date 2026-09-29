"""Capture grading: per-octave-band SNR and repeat-sweep consistency.

Each band gets PASS / MARGINAL / REDO. Bands well below the PA's
passband level (its roll-off) are marked out of range and not graded, so
a small PA that can't make 31.5 Hz isn't told to redo forever. The overall
grade is the worst graded band, and the reasons group neighbouring bands
into readable ranges ("low-end noise too high below 88 Hz").
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum

import numpy as np

from .spectrum import OCTAVE_CENTERS, OCTAVE_NOMINAL, band_sum, format_hz, octave_edges


class Grade(IntEnum):
    PASS = 0
    MARGINAL = 1
    REDO = 2

    @property
    def label(self) -> str:
        return self.name.lower()


@dataclass(frozen=True)
class GradingConfig:
    snr_pass_db: float = 20.0
    snr_marginal_db: float = 10.0
    consistency_pass_db: float = 1.0
    consistency_marginal_db: float = 3.0
    # A band this far below the passband level is outside the PA's range.
    out_of_range_db: float = 10.0
    passband: tuple[float, float] = (250.0, 4000.0)


@dataclass
class BandResult:
    name: str
    center: float
    lo: float
    hi: float
    level_db: float               # band level relative to the passband level
    snr_db: float
    spread_db: float | None       # repeat-to-repeat level spread in the worst 1/3 octave
    excess_spread_db: float | None  # ... minus what the measured noise can explain (graded)
    out_of_range: bool
    snr_grade: Grade
    consistency_grade: Grade

    @property
    def grade(self) -> Grade | None:
        if self.out_of_range:
            return None
        return max(self.snr_grade, self.consistency_grade)


@dataclass
class CaptureGrade:
    bands: list[BandResult]
    overall: Grade
    reasons: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def band_grades(self) -> list[Grade | None]:
        return [b.grade for b in self.bands]


def _grade(value: float, pass_at: float, marginal_at: float, higher_is_better: bool) -> Grade:
    if higher_is_better:
        return Grade.PASS if value >= pass_at else Grade.MARGINAL if value >= marginal_at else Grade.REDO
    return Grade.PASS if value <= pass_at else Grade.MARGINAL if value <= marginal_at else Grade.REDO


def grade_capture(freqs: np.ndarray, signal_power: np.ndarray, noise_power: np.ndarray,
                  repeat_powers: list[np.ndarray] | None = None,
                  cfg: GradingConfig = GradingConfig(), f_max: float | None = None) -> CaptureGrade:
    """Grade one capture.

    signal_power / noise_power are per-bin powers in the same units
    (|H|^2 domain). repeat_powers holds |H_i|^2 for each repeat sweep
    when there are two or more. f_max clips the top band (sweep f2 / Nyquist).
    """
    f_top = f_max if f_max is not None else freqs[-1]
    pb_sig, pb_n = band_sum(freqs, signal_power, *cfg.passband)
    passband_density = max(pb_sig, 1e-30) / max(pb_n, 1)

    bands: list[BandResult] = []
    for name, fc in zip(OCTAVE_NOMINAL, OCTAVE_CENTERS):
        lo, hi = octave_edges(fc)
        hi = min(hi, f_top)
        s, n = band_sum(freqs, signal_power, lo, hi)
        nz, _ = band_sum(freqs, noise_power, lo, hi)
        s = max(s, 1e-30)
        level_db = 10 * np.log10((s / max(n, 1)) / passband_density)
        snr_db = 10 * np.log10(s / max(nz, 1e-30))

        spread = excess = None
        if repeat_powers is not None and len(repeat_powers) > 1:
            spread, excess = _worst_third_octave_spread(freqs, repeat_powers, signal_power, noise_power, lo, hi)

        bands.append(BandResult(
            name=name, center=fc, lo=lo, hi=hi, level_db=float(level_db), snr_db=float(snr_db),
            spread_db=spread, excess_spread_db=excess, out_of_range=False,
            snr_grade=_grade(snr_db, cfg.snr_pass_db, cfg.snr_marginal_db, True),
            consistency_grade=(Grade.PASS if excess is None else
                               _grade(excess, cfg.consistency_pass_db, cfg.consistency_marginal_db, False)),
        ))

    return _verdict(bands, cfg)


def regrade(grade: CaptureGrade, freqs: np.ndarray, signal_power: np.ndarray,
            cfg: GradingConfig = GradingConfig()) -> CaptureGrade:
    """The same capture graded against other settings (a sub's reference band):
    exactly what grade_capture would give with them. SNR and repeat spread
    don't depend on the band, so they're kept; the levels, the out-of-range
    ends and the verdict are redone. Notes that didn't come from grading
    (clock drift) are kept."""
    pb_sig, pb_n = band_sum(freqs, signal_power, *cfg.passband)
    passband_density = max(pb_sig, 1e-30) / max(pb_n, 1)
    bands = []
    for b in grade.bands:
        s, n = band_sum(freqs, signal_power, b.lo, b.hi)
        s = max(s, 1e-30)
        bands.append(BandResult(
            name=b.name, center=b.center, lo=b.lo, hi=b.hi,
            level_db=float(10 * np.log10((s / max(n, 1)) / passband_density)), snr_db=b.snr_db,
            spread_db=b.spread_db, excess_spread_db=b.excess_spread_db, out_of_range=False,
            snr_grade=_grade(b.snr_db, cfg.snr_pass_db, cfg.snr_marginal_db, True),
            consistency_grade=(Grade.PASS if b.excess_spread_db is None else
                               _grade(b.excess_spread_db, cfg.consistency_pass_db, cfg.consistency_marginal_db, False)),
        ))
    result = _verdict(bands, cfg)
    result.notes += grade.notes[len(_range_notes(grade.bands)):]
    return result


def _verdict(bands: list[BandResult], cfg: GradingConfig) -> CaptureGrade:
    # Out of range = contiguous run of low-level bands at either end of the spectrum.
    for order in (range(len(bands)), reversed(range(len(bands)))):
        for i in order:
            if bands[i].level_db < -cfg.out_of_range_db:
                bands[i].out_of_range = True
            else:
                break

    graded = [b for b in bands if not b.out_of_range]
    overall = max((b.grade for b in graded), default=Grade.REDO)
    result = CaptureGrade(bands=bands, overall=overall)
    result.reasons = _reasons(bands, cfg)
    result.notes = _range_notes(bands)
    if not graded:
        result.reasons = ["no usable signal: check the mic is connected and the PA is playing"]
    elif overall == Grade.PASS:
        result.reasons = [f"all bands ≥ {cfg.snr_pass_db:.0f} dB SNR" +
                          (f", repeats within {cfg.consistency_pass_db:g} dB"
                           if any(b.spread_db is not None for b in graded) else "")]
    return result


def _worst_third_octave_spread(freqs: np.ndarray, repeat_powers: list[np.ndarray], signal: np.ndarray,
                               noise: np.ndarray, lo: float, hi: float) -> tuple[float, float]:
    """Repeat-to-repeat level spread per 1/3 octave (the display resolution).

    Steady noise alone makes repeats wobble: with n bins at per-repeat SNR s,
    each repeat's band level has std ~ 4.34 * sqrt(2 / (n s)) dB. Only the
    spread beyond 3 sigma of that (for a pair) is attributed to an event
    such as a bang or the mic moving. Returns (raw, excess) for the third
    with the largest excess. A burst that shifts one third by 2 dB barely
    moves the whole octave, hence thirds.
    """
    k = len(repeat_powers)
    worst_raw, worst_excess = 0.0, -np.inf
    edges = lo * 2.0 ** (np.arange(4) / 3)
    for a, b in zip(edges[:-1], edges[1:]):
        b = min(b, hi)
        if b <= a:
            break
        levels = [10 * np.log10(max(band_sum(freqs, p, a, b)[0], 1e-30)) for p in repeat_powers]
        raw = float(np.max(levels) - np.min(levels))
        s, n_bins = band_sum(freqs, signal, a, b)
        nz, _ = band_sum(freqs, noise, a, b)
        snr_single = max(s, 1e-30) / max(nz * k, 1e-30)    # noise of one repeat is k x the average's
        sigma = 4.34 * np.sqrt(2.0 / (max(n_bins, 1) * snr_single))
        excess = raw - 3 * np.sqrt(2) * sigma
        if excess > worst_excess:
            worst_raw, worst_excess = raw, excess
    return worst_raw, max(0.0, float(worst_excess))


def _runs(bands: list[BandResult], key) -> list[list[BandResult]]:
    """Contiguous runs of bands whose key(band) is a non-PASS grade."""
    runs, current = [], []
    for b in bands:
        g = None if b.out_of_range else key(b)
        if g is not None and g > Grade.PASS:
            current.append(b)
        elif current:
            runs.append(current)
            current = []
    if current:
        runs.append(current)
    return runs


def _span(run: list[BandResult], graded: list[BandResult]) -> str:
    lo, hi = format_hz(run[0].lo), format_hz(run[-1].hi)
    at_bottom = run[0] is graded[0]
    at_top = run[-1] is graded[-1]
    if at_bottom and not at_top:
        return f"below {hi}"
    if at_top and not at_bottom:
        return f"above {lo}"
    return f"{lo}–{hi}"


def _reasons(bands: list[BandResult], cfg: GradingConfig) -> list[str]:
    graded = [b for b in bands if not b.out_of_range]
    if not graded:
        return []
    reasons = []
    for run in _runs(bands, lambda b: b.snr_grade):
        worst = min(b.snr_db for b in run)
        severity = Grade(max(b.snr_grade for b in run)).label
        span = _span(run, graded)
        where = ("low-end " if span.startswith("below") else
                 "high-frequency " if span.startswith("above") else "")
        reasons.append(f"{severity}: {where}noise too high {span} (SNR {worst:.0f} dB)")
    # Low SNR makes repeats disagree too; only report inconsistency that
    # steady noise doesn't already explain.
    for run in _runs(bands, lambda b: b.consistency_grade if b.consistency_grade > b.snr_grade else Grade.PASS):
        # Report the raw spread (what the user would see comparing repeats).
        worst = max(b.spread_db for b in run)
        severity = Grade(max(b.consistency_grade for b in run)).label
        reasons.append(f"{severity}: repeat sweeps differ by {worst:.1f} dB {_span(run, graded)} "
                       f"(noise burst or movement during a sweep?)")
    return reasons


def _range_notes(bands: list[BandResult]) -> list[str]:
    # The out-of-range runs at each end (not halves of the spectrum: a sub's
    # range sits at the bottom, so its high run starts in the low half).
    n_low = 0
    while n_low < len(bands) and bands[n_low].out_of_range:
        n_low += 1
    n_high = 0
    while n_high < len(bands) - n_low and bands[-1 - n_high].out_of_range:
        n_high += 1
    notes = []
    if n_low:
        notes.append(f"below the PA's range under {format_hz(bands[n_low - 1].hi)} (not graded)")
    if n_high:
        notes.append(f"above the PA's range over {format_hz(bands[len(bands) - n_high].lo)} (not graded)")
    return notes
