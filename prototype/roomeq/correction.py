"""Fit a conservative minimum-phase correction to the averaged measurement.

Pipeline (design_correction):

1. The averaged response at the session's smoothing, its 1-octave trend and a
   1/6-octave version for null detection, all on a 24-points-per-octave grid.
2. Fit range: the PA's -6 dB points (1-octave trend vs. the 250 Hz-4 kHz
   mean), intersected with the user's frequency range. Nothing is corrected
   outside it, and nothing is ever boosted outside it.
3. Nulls: where the average dips more than 6 dB below its 1-octave trend, or
   the positions (1/3 octave, level-aligned) disagree by more than 6 dB. The
   correction may cut there but never boost.
4. Desired correction = target - average, clipped to [-max cut, +max boost]
   (0 in nulls for boosts, 0 outside the range).
5. Bands are added greedily (bell on the largest-area residual, or a low /
   high shelf, whichever helps most), and all bands are refined together with
   a bounded Levenberg-Marquardt fit after each addition. Width limits keep
   it to broad trends: boosts at least 1 octave wide, cuts at least 1/3
   octave below ~300 Hz and 2/3 octave above.
6. Quick mode: the fit limits are raised to cap/strength, then every gain is
   scaled by the strength, so the applied correction never exceeds the cap.

The optimiser is written out (no scipy) so the C++ port can follow it step
for step.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass, field

import numpy as np

from . import filters
from .averaging import SessionSummary, band_mask_weights, usable_range
from .filters import BELL, HIGH_SHELF, LOW_SHELF, SHELF_Q, Band
from .grading import Grade
from .spectrum import log_freq_grid, rebin_power, smooth_power, to_db
from .targets import TargetCurve, anchor_offset_db


@dataclass(frozen=True)
class CorrectionConfig:
    max_bands: int = 10
    max_cut_db: float = 12.0
    max_boost_db: float = 3.0
    range_hz: tuple[float, float] = (20.0, 20000.0)   # user frequency range, intersected with the PA's
    rolloff_db: float = 6.0                            # PA range edge: this far below the 250 Hz-4 kHz mean
    boost_min_octaves: float = 1.0
    cut_min_octaves_low: float = 1 / 3                 # below `cut_split_hz` (room modes that survive averaging)
    cut_min_octaves_high: float = 2 / 3
    cut_split_hz: tuple[float, float] = (250.0, 360.0)  # log-interpolated between the two widths
    max_octaves: float = 3.0                           # widest bell
    null_dip_db: float = 6.0
    null_spread_db: float = 6.0
    null_margin_octaves: float = 1 / 6
    points_per_octave: int = 24
    outside_weight: float = 0.05                       # slight pull towards 0 dB outside the fit range
    penalty: float = 100.0                             # weight on exceeding the limits
    min_gain_db: float = 0.5                           # smaller bands are dropped
    min_improvement: float = 0.02                      # a new band must cut the fit cost by this fraction
    stop_rms_db: float = 0.5                           # stop adding bands below this in-range RMS error


@dataclass
class FitProblem:
    """Everything the band fit needs, on the fit grid."""
    freqs: np.ndarray
    fs: float
    desired: np.ndarray          # dB, clipped; 0 outside the range
    weight: np.ndarray           # per point (0 where the average is missing)
    upper: np.ndarray            # max allowed correction per point (dB)
    lower: np.ndarray            # min allowed correction per point (dB, negative)
    f_lo: float
    f_hi: float
    max_cut: float
    max_boost: float
    cfg: CorrectionConfig


@dataclass
class CorrectionResult:
    grid: np.ndarray
    average_db: np.ndarray           # the average the fit worked from
    trend_db: np.ndarray             # 1-octave trend (range and null detection)
    target_db: np.ndarray            # anchored target
    desired_db: np.ndarray           # clipped target - average (0 outside the range)
    upper_db: np.ndarray
    lower_db: np.ndarray
    null_mask: np.ndarray
    fit_range: tuple[float, float]
    fitted: list[Band]               # full strength, within the (cap-adjusted) limits
    bands: list[Band]                # scaled by the quick-mode strength: what the audio path gets
    strength: float
    correction_db: np.ndarray        # response of `bands`
    predicted_db: np.ndarray         # average + correction
    rms_error_db: float              # predicted vs target, in range, outside nulls
    notes: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Band parameterisation and bounds

def _min_cut_octaves(f: float, cfg: CorrectionConfig) -> float:
    a, b = np.log2(cfg.cut_split_hz[0]), np.log2(cfg.cut_split_hz[1])
    t = float(np.clip((np.log2(f) - a) / (b - a), 0.0, 1.0))
    return cfg.cut_min_octaves_low + t * (cfg.cut_min_octaves_high - cfg.cut_min_octaves_low)


def q_max(f: float, gain: float, cfg: CorrectionConfig) -> float:
    octaves = cfg.boost_min_octaves if gain > 0 else _min_cut_octaves(f, cfg)
    return filters.q_for_bandwidth(octaves)


def _from_params(kind: str, p) -> Band:
    q = 2.0 ** p[2] if kind == BELL else SHELF_Q
    return Band(kind, float(2.0 ** p[0]), float(p[1]), float(q))


def _project(kind: str, p: np.ndarray, sign: float, prob: FitProblem) -> np.ndarray:
    p = p.copy()
    p[0] = np.clip(p[0], np.log2(prob.f_lo), np.log2(prob.f_hi))
    p[1] = np.clip(p[1], 0.0, prob.max_boost) if sign > 0 else np.clip(p[1], -prob.max_cut, 0.0)
    if kind == BELL:
        q_lo = filters.q_for_bandwidth(prob.cfg.max_octaves)
        p[2] = np.clip(p[2], np.log2(q_lo), np.log2(q_max(2.0 ** p[0], sign, prob.cfg)))
    return p


@dataclass
class _Slot:
    kind: str
    sign: float
    p: np.ndarray

    def band(self) -> Band:
        return _from_params(self.kind, self.p)


# ---------------------------------------------------------------------------
# Least squares

def _residual(prob: FitProblem, c: np.ndarray) -> np.ndarray:
    sp = np.sqrt(prob.cfg.penalty)
    return np.concatenate([
        np.sqrt(prob.weight) * (c - prob.desired),
        sp * np.maximum(0.0, c - prob.upper),
        sp * np.maximum(0.0, prob.lower - c),
    ])


def _slot_db(s: _Slot, prob: FitProblem) -> np.ndarray:
    return filters.band_db(s.band(), prob.freqs, prob.fs)


def _cost(prob: FitProblem, c: np.ndarray) -> float:
    r = _residual(prob, c)
    return 0.5 * float(r @ r)


def levenberg_marquardt(slots: list[_Slot], free: list[int], prob: FitProblem, fixed_db: np.ndarray,
                        max_iter: int = 100) -> float:
    """Refine the parameters of slots[free] (in place); the others contribute `fixed_db`.

    Bounded LM: steps are projected onto the bounds; lambda scales the
    diagonal of J^T J (Marquardt). Returns the final cost.
    """
    responses = {i: _slot_db(slots[i], prob) for i in free}
    c = fixed_db + sum(responses.values())
    cost = _cost(prob, c)
    lam = 1e-2
    sp = np.sqrt(prob.cfg.penalty)
    sw = np.sqrt(prob.weight)
    for _ in range(max_iter):
        # Jacobian of the correction curve w.r.t. each free parameter (forward differences).
        cols = []
        for i in free:
            s = slots[i]
            for k in range(len(s.p)):
                h = 1e-6
                q = s.p.copy()
                q[k] += h
                d = (filters.band_db(_from_params(s.kind, q), prob.freqs, prob.fs) - responses[i]) / h
                cols.append(d)
        dc = np.array(cols).T                                  # (points, params)
        over = (c > prob.upper).astype(float)
        under = (prob.lower > c).astype(float)
        jac = np.vstack([sw[:, None] * dc, sp * over[:, None] * dc, -sp * under[:, None] * dc])
        r = _residual(prob, c)
        a = jac.T @ jac
        g = jac.T @ r
        improved = False
        while lam < 1e10:
            step = np.linalg.solve(a + lam * np.diag(np.diag(a) + 1e-9), -g)
            trial = {}
            pos = 0
            for i in free:
                n = len(slots[i].p)
                trial[i] = _project(slots[i].kind, slots[i].p + step[pos:pos + n], slots[i].sign, prob)
                pos += n
            trial_resp = {i: filters.band_db(_from_params(slots[i].kind, trial[i]), prob.freqs, prob.fs)
                          for i in free}
            c_new = fixed_db + sum(trial_resp.values())
            cost_new = _cost(prob, c_new)
            if cost_new < cost:
                for i in free:
                    slots[i].p = trial[i]
                responses = trial_resp
                decrease = cost - cost_new
                c, cost = c_new, cost_new
                lam = max(lam / 3, 1e-9)
                improved = True
                break
            lam *= 4
        if not improved or decrease <= 1e-9 * max(cost, 1e-12):
            break
    return cost


# ---------------------------------------------------------------------------
# Greedy band seeding

def _seed_bell(prob: FitProblem, resid: np.ndarray) -> _Slot | None:
    inside = (prob.weight > 0) & (prob.freqs >= prob.f_lo) & (prob.freqs <= prob.f_hi)
    x = np.log2(prob.freqs)
    best = None
    i = 0
    n = len(resid)
    while i < n:
        if not inside[i] or abs(resid[i]) < 0.1:
            i += 1
            continue
        sgn = np.sign(resid[i])
        j = i
        while j + 1 < n and inside[j + 1] and np.sign(resid[j + 1]) == sgn and abs(resid[j + 1]) >= 0.1:
            j += 1
        area = float(np.sum(np.abs(resid[i:j + 1]))) / prob.cfg.points_per_octave
        if best is None or area > best[0]:
            best = (area, i, j, sgn)
        i = j + 1
    if best is None:
        return None
    _, i, j, sgn = best
    k = i + int(np.argmax(np.abs(resid[i:j + 1])))
    half = abs(resid[k]) / 2
    a = k
    while a > i and abs(resid[a - 1]) >= half:
        a -= 1
    b = k
    while b < j and abs(resid[b + 1]) >= half:
        b += 1
    octaves = max(x[b] - x[a] + 1.0 / prob.cfg.points_per_octave, 0.1)
    p = np.array([x[k], resid[k], np.log2(filters.q_for_bandwidth(octaves))])
    return _Slot(BELL, float(sgn), _project(BELL, p, float(sgn), prob))


def _seed_shelves(prob: FitProblem, resid: np.ndarray, kind: str) -> list[_Slot]:
    """Candidate shelves at 1/3-octave steps, gain = mean residual on the shelved side."""
    inside = (prob.weight > 0) & (prob.freqs >= prob.f_lo) & (prob.freqs <= prob.f_hi)
    out = []
    for f0 in prob.f_lo * 2.0 ** (np.arange(1, 30) / 3):
        if f0 >= prob.f_hi:
            break
        side = inside & ((prob.freqs <= f0) if kind == LOW_SHELF else (prob.freqs >= f0))
        if np.count_nonzero(side) < prob.cfg.points_per_octave // 2:
            continue
        g = float(np.mean(resid[side]))
        if abs(g) < 0.3:
            continue
        sgn = float(np.sign(g))
        out.append(_Slot(kind, sgn, _project(kind, np.array([np.log2(f0), g]), sgn, prob)))
    return out


def fit_bands(prob: FitProblem) -> list[Band]:
    slots: list[_Slot] = []
    zero = np.zeros(len(prob.freqs))

    def curve(ss):
        return zero + sum((_slot_db(s, prob) for s in ss), zero)

    inside = (prob.weight > 0) & (prob.freqs >= prob.f_lo) & (prob.freqs <= prob.f_hi)
    if not np.any(inside):
        return []
    cost = _cost(prob, zero)
    for _ in range(prob.cfg.max_bands):
        c = curve(slots)
        resid = prob.desired - c
        if np.sqrt(np.mean(resid[inside] ** 2)) < prob.cfg.stop_rms_db:
            break
        candidates = []
        bell = _seed_bell(prob, resid)
        if bell is not None:
            candidates.append(bell)
        kinds = {s.kind for s in slots}
        for kind in (LOW_SHELF, HIGH_SHELF):
            if kind not in kinds:
                candidates += _seed_shelves(prob, resid, kind)
        if not candidates:
            break
        # Refine each candidate on its own against the current bands; keep the best.
        best = None
        for cand in candidates:
            trial = _Slot(cand.kind, cand.sign, cand.p.copy())
            cst = levenberg_marquardt([trial], [0], prob, c, max_iter=30)
            if best is None or cst < best[0]:
                best = (cst, trial)
        if best is None or best[0] > cost * (1 - prob.cfg.min_improvement):
            break
        slots.append(best[1])
        cost = levenberg_marquardt(slots, list(range(len(slots))), prob, zero)

    # Drop negligible bands, then refit what's left.
    kept = [s for s in slots if abs(s.p[1]) >= prob.cfg.min_gain_db]
    if len(kept) != len(slots) and kept:
        levenberg_marquardt(kept, list(range(len(kept))), prob, zero)
    return sorted((s.band() for s in kept), key=lambda b: b.freq)


# ---------------------------------------------------------------------------
# Session -> correction

def _aligned_positions(captures, summary: SessionSummary, fraction: float, grid: np.ndarray) -> list[np.ndarray]:
    """Included, non-redo captures, level-aligned and smoothed, redo bands masked."""
    freqs = summary.freqs
    out = []
    for c, off in zip(captures, summary.offsets_db):
        if c.excluded or c.grade.overall == Grade.REDO:
            continue
        w = band_mask_weights(c.freqs, c.grade) * c.weight
        p = c.power
        if len(c.freqs) != len(freqs):
            p, w = rebin_power(c.freqs, p, freqs), rebin_power(c.freqs, w, freqs)
        out.append(to_db(smooth_power(freqs, p * 10 ** (off / 10), fraction, grid, w)))
    return out


def detect_nulls(grid: np.ndarray, fine_db: np.ndarray, trend_db: np.ndarray, positions: list[np.ndarray],
                 cfg: CorrectionConfig) -> np.ndarray:
    null = fine_db - trend_db < -cfg.null_dip_db
    if len(positions) >= 2:
        stack = np.array(positions)
        valid = np.sum(np.isfinite(stack), axis=0) >= 2
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)      # all-NaN columns
            spread = np.nanmax(stack, axis=0) - np.nanmin(stack, axis=0)
        null |= valid & (spread > cfg.null_spread_db)
    # Widen each null by the margin on both sides.
    margin = int(round(cfg.null_margin_octaves * cfg.points_per_octave))
    if margin > 0 and np.any(null):
        idx = np.flatnonzero(null)
        wide = np.zeros_like(null)
        for i in idx:
            wide[max(0, i - margin):i + margin + 1] = True
        null = wide
    return null


def design_correction(captures, summary: SessionSummary, target: TargetCurve, fs: float,
                      cfg: CorrectionConfig = CorrectionConfig()) -> CorrectionResult:
    grid = log_freq_grid(20.0, 20000.0, cfg.points_per_octave)
    policy = summary.policy
    average = to_db(smooth_power(summary.freqs, summary.power, policy.smoothing_fraction, grid, summary.weight))
    fine = to_db(smooth_power(summary.freqs, summary.power, 6, grid, summary.weight))
    trend = to_db(smooth_power(summary.freqs, summary.power, 1, grid, summary.weight))
    notes = []

    lo, hi = usable_range(grid, trend, drop_db=cfg.rolloff_db)
    lo, hi = max(lo, cfg.range_hz[0]), min(hi, cfg.range_hz[1])
    notes.append(f"fit range {lo:.0f} Hz - {hi:.0f} Hz")

    offset = anchor_offset_db(grid, average, target)
    target_db = offset + target.db(grid)

    strength = policy.strength
    max_cut, max_boost = cfg.max_cut_db, cfg.max_boost_db
    if policy.max_correction_db is not None:
        max_cut = min(max_cut, policy.max_correction_db / strength)
        max_boost = min(max_boost, policy.max_correction_db / strength)

    positions = _aligned_positions(captures, summary, 3, grid)
    null = detect_nulls(grid, fine, trend, positions, cfg)

    inside = (grid >= lo) & (grid <= hi)
    finite = np.isfinite(average)
    upper = np.where(inside & ~null, max_boost, 0.0)
    lower = np.full(len(grid), -max_cut)
    desired = np.where(inside & finite, np.clip(np.nan_to_num(target_db - average), lower, upper), 0.0)
    weight = np.where(inside, np.where(finite, 1.0, 0.0), cfg.outside_weight)

    prob = FitProblem(freqs=grid, fs=fs, desired=desired, weight=weight, upper=upper, lower=lower,
                      f_lo=lo, f_hi=hi, max_cut=max_cut, max_boost=max_boost, cfg=cfg)
    fitted = fit_bands(prob)
    bands = [b.scaled(strength) for b in fitted]
    correction = filters.response_db(bands, grid, fs)
    predicted = average + correction
    judge = inside & ~null & finite
    err = predicted - target_db
    rms = float(np.sqrt(np.mean(err[judge] ** 2))) if np.any(judge) else float("nan")
    if strength < 1:
        notes.append(f"quick mode: {strength:.0%} strength, capped at {policy.max_correction_db:g} dB")
    return CorrectionResult(grid=grid, average_db=average, trend_db=trend, target_db=target_db, desired_db=desired,
                            upper_db=upper, lower_db=lower, null_mask=null, fit_range=(lo, hi), fitted=fitted,
                            bands=bands, strength=strength, correction_db=correction, predicted_db=predicted,
                            rms_error_db=rms, notes=notes)
