"""Phase 3: level calibration, level tracking and loudness compensation.

Chain: the plugin output (after correction and voicing, before this stage)
is C-weighted and its level tracked; a calibration maps that level to SPL
at the mix position; the ISO 226 contour difference between the current
SPL and the reference SPL is applied as a low shelf and a high shelf.

- Compensation target: D(f) = [Lp(f, now) - now] - [Lp(f, ref) - ref], zero
  at or above the reference. It keeps almost the same shape as the level
  drops and scales with the drop, so the shelves' frequency and Q are fitted
  once per reference level and only their gains follow the level (a table of
  gains against dB below reference). Gains glide; nothing is refitted live.
- Limits: amount, max low boost, max high boost, and a fixed low-boost
  ceiling nothing can exceed.
- Tracking: 400 ms C-weighted momentary level; the estimate rises as a ~1 s
  energy average (boost backs off quickly when it gets louder) and falls in
  dB with `speed` as the time constant (boost grows slowly when it gets
  quieter). Silence, and
  anything more than 20 dB below the estimate, is a pause and holds the
  estimate - but a drop that lasts longer than `pause_hold_s` is real and is
  followed.
- Optional protective high-pass (24 dB/octave) at the PA's measured low-end
  roll-off, rising up to half an octave as the low boost reaches its maximum.
- Deadband: the compensation's level only moves when the estimate pushes
  more than 2 dB past it, and otherwise drifts towards it over ~30 s, so the
  EQ doesn't wander with the music's dynamics but still ends up exactly on a
  real change.
- Re-check: ~10 s of program, output vs mic, re-anchors the calibration if
  the gain after the plugin changed (amps, a fader after it). It compares the
  output-to-mic transfer function per third octave with the one stored at
  calibration (dual-FFT H1, so crowd noise, uncorrelated with the output,
  drops out); the median change is the gain change.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import scipy.signal as ss

from . import dualfft, filters, iso226
from .filters import HIGH_PASS, HIGH_SHELF, LOW_SHELF, Band


@dataclass(frozen=True)
class LoudnessConfig:
    reference_spl: float = 95.0       # dB(C) at the mix position where the system sounds right
    amount: float = 1.0
    max_low_db: float = 8.0
    max_high_db: float = 4.0
    low_ceiling_db: float = 12.0      # fixed: never more low boost than this
    speed_s: float = 5.0              # falling-level time constant
    attack_s: float = 1.0             # rising-level time constant
    window_s: float = 0.4             # momentary level window
    abs_gate_dbfs: float = -70.0      # below this (C-weighted) it's silence
    rel_gate_db: float = 20.0         # this far below the estimate it's a pause...
    pause_hold_s: float = 8.0         # ...unless it lasts longer than this
    hp_track: bool = False
    hp_rise_octaves: float = 0.5
    deadband_db: float = 2.0          # level changes smaller than this don't move the EQ...
    drift_s: float = 30.0             # ...except by a slow drift that lands it on the level


# ---------------------------------------------------------------------------
# Weighting and targets

def c_weighting_sos(fs: float) -> np.ndarray:
    """IEC 61672 C-weighting: 2 poles at 20.6 Hz (high-pass) and 2 at 12194 Hz
    (low-pass), as two RBJ sections with Q 0.5, normalised to 0 dB at 1 kHz."""
    sos = filters.sos([Band(HIGH_PASS, 20.598997, q=0.5), Band(filters.LOW_PASS, 12194.217, q=0.5)], fs)
    gain = 10 ** (-filters.response_db([Band(HIGH_PASS, 20.598997, q=0.5), Band(filters.LOW_PASS, 12194.217, q=0.5)],
                                       np.array([1000.0]), fs)[0] / 20)
    sos = sos.copy()
    sos[0, :3] *= gain
    return sos


def compensation_target(freqs: np.ndarray, current_spl: float, reference_spl: float) -> np.ndarray:
    """ISO 226 contour difference (dB) to keep the balance heard at the reference level."""
    if current_spl >= reference_spl:
        return np.zeros(len(freqs))
    now = max(current_spl, 20.0)            # the contours aren't defined below 20 phon
    return iso226.relative_contour(freqs, now) - iso226.relative_contour(freqs, reference_spl)


# ---------------------------------------------------------------------------
# Shelves

LOW_FIT = (20.0, 1000.0)
HIGH_FIT = (2000.0, 16000.0)
PLAN_DELTAS = np.arange(0.0, 61.0, 1.0)     # dB below reference
PLAN_FIT_DELTA = 15.0                       # shelf frequency/Q fitted here


@dataclass
class ShelfPlan:
    reference_spl: float
    low_freq: float
    low_q: float
    high_freq: float
    high_q: float
    deltas: np.ndarray        # dB below reference
    low_gain: np.ndarray      # fitted gain at each delta (amount 100%, no limits)
    high_gain: np.ndarray


def _fit_gain(kind: str, freq: float, q: float, target: np.ndarray, grid: np.ndarray, fs: float) -> float:
    """Least-squares gain for a shelf of fixed frequency/Q. A shelf's dB response
    scales almost exactly with its gain, so one linear solve plus one refinement."""
    g = 1.0
    for _ in range(2):
        shape = filters.band_db(Band(kind, freq, g, q), grid, fs) / g
        g_new = float(np.dot(shape, target) / np.dot(shape, shape))
        if abs(g_new) < 1e-6:
            return 0.0
        g = g_new
    return max(g, 0.0)


def plan_shelves(reference_spl: float, fs: float = 48000.0) -> ShelfPlan:
    """Fit shelf frequency/Q at a typical drop, then the gains for every drop."""
    low_grid = np.geomspace(*LOW_FIT, 60)
    high_grid = np.geomspace(*HIGH_FIT, 30)
    t_low = compensation_target(low_grid, reference_spl - PLAN_FIT_DELTA, reference_spl)
    t_high = compensation_target(high_grid, reference_spl - PLAN_FIT_DELTA, reference_spl)

    best = None
    for f0 in np.geomspace(60.0, 600.0, 41):
        for q in (0.4, 0.5, 0.6, 0.7071):
            g = _fit_gain(LOW_SHELF, f0, q, t_low, low_grid, fs)
            err = np.sum((filters.band_db(Band(LOW_SHELF, f0, g, q), low_grid, fs) - t_low) ** 2)
            if best is None or err < best[0]:
                best = (err, f0, q)
    _, low_f, low_q = best
    best = None
    for f0 in np.geomspace(4000.0, 16000.0, 25):
        g = _fit_gain(HIGH_SHELF, f0, filters.SHELF_Q, t_high, high_grid, fs)
        err = np.sum((filters.band_db(Band(HIGH_SHELF, f0, g, filters.SHELF_Q), high_grid, fs) - t_high) ** 2)
        if best is None or err < best[0]:
            best = (err, f0)
    _, high_f = best

    low_gains, high_gains = [], []
    for d in PLAN_DELTAS:
        low_gains.append(_fit_gain(LOW_SHELF, low_f, low_q,
                                   compensation_target(low_grid, reference_spl - d, reference_spl), low_grid, fs) if d > 0 else 0.0)
        high_gains.append(_fit_gain(HIGH_SHELF, high_f, filters.SHELF_Q,
                                    compensation_target(high_grid, reference_spl - d, reference_spl), high_grid, fs) if d > 0 else 0.0)
    return ShelfPlan(reference_spl, float(low_f), float(low_q), float(high_f), filters.SHELF_Q,
                     PLAN_DELTAS.copy(), np.array(low_gains), np.array(high_gains))


def shelf_gains(plan: ShelfPlan, current_spl: float, cfg: LoudnessConfig) -> tuple[float, float]:
    """Low and high shelf gains (dB) at the current level, with amount and limits applied."""
    delta = plan.reference_spl - current_spl
    if delta <= 0:
        return 0.0, 0.0
    low = float(np.interp(delta, plan.deltas, plan.low_gain)) * cfg.amount
    high = float(np.interp(delta, plan.deltas, plan.high_gain)) * cfg.amount
    return min(low, cfg.max_low_db, cfg.low_ceiling_db), min(high, cfg.max_high_db)


def loudness_bands(plan: ShelfPlan, low_gain: float, high_gain: float, cfg: LoudnessConfig,
                   hp_base_hz: float | None) -> list[Band]:
    """The stage's filters: low shelf, high shelf, and the optional tracking high-pass."""
    bands = [Band(LOW_SHELF, plan.low_freq, low_gain, plan.low_q),
             Band(HIGH_SHELF, plan.high_freq, high_gain, plan.high_q)]
    if cfg.hp_track and hp_base_hz is not None:
        bands += tracking_highpass(hp_base_hz, low_gain, cfg)
    return bands


def tracking_highpass(base_hz: float, low_gain: float, cfg: LoudnessConfig) -> list[Band]:
    """24 dB/octave Butterworth at the PA's roll-off, up to hp_rise_octaves higher at max low boost."""
    top = max(min(cfg.max_low_db, cfg.low_ceiling_db), 1e-6)
    fc = base_hz * 2 ** (cfg.hp_rise_octaves * np.clip(low_gain / top, 0.0, 1.0))
    return [Band(HIGH_PASS, fc, q=0.5411961), Band(HIGH_PASS, fc, q=1.3065630)]


# ---------------------------------------------------------------------------
# Calibration and tracking

@dataclass(frozen=True)
class Calibration:
    """Output level (C-weighted dBFS at the loudness stage's input) that gave `spl` dB(C) at the mix position."""
    output_dbfs: float
    spl: float
    mic_dbfs: float | None = None      # mic level (C-weighted) during calibration, for mic tracking

    def spl_from_output(self, level_dbfs: float) -> float:
        return self.spl + level_dbfs - self.output_dbfs

    def spl_from_mic(self, level_dbfs: float) -> float | None:
        return None if self.mic_dbfs is None else self.spl + level_dbfs - self.mic_dbfs


def c_weighted_level_dbfs(x: np.ndarray, fs: float) -> float:
    y = ss.sosfilt(c_weighting_sos(fs), x)
    return 10 * np.log10(np.mean(y ** 2) + 1e-30)


class LevelTracker:
    """Block-by-block level estimate (C-weighted dBFS), as the audio thread will run it."""

    def __init__(self, fs: float, cfg: LoudnessConfig):
        self.fs, self.cfg = fs, cfg
        self.sos = c_weighting_sos(fs)
        self.zi = np.zeros((self.sos.shape[0], 2))
        self.window = int(round(cfg.window_s * fs))
        self.acc = 0.0
        self.count = 0
        self.estimate: float | None = None      # dBFS, None until the first non-silent window
        self.gated_for = 0.0                    # seconds continuously below the relative gate
        self.last_active = False                # did the last window count (program playing)?

    def process(self, block: np.ndarray, follow: "LevelTracker | None" = None) -> None:
        """Feed a block. With `follow`, a window only counts when the followed
        tracker's matching window did (mic tracking: only while program plays,
        so crowd noise in the pauses isn't counted). Feed `follow` the same
        block first."""
        y, self.zi = ss.sosfilt(self.sos, block, zi=self.zi)
        pos = 0
        while pos < len(y):
            take = min(self.window - self.count, len(y) - pos)
            self.acc += float(np.sum(y[pos:pos + take] ** 2))
            self.count += take
            pos += take
            if self.count == self.window:
                momentary = 10 * np.log10(self.acc / self.window + 1e-30)
                if follow is None:
                    self.last_active = self._update(momentary)
                elif follow.last_active:
                    self._update(momentary, gates=False)
                self.acc, self.count = 0.0, 0

    def _update(self, momentary: float, gates: bool = True) -> bool:
        """Returns whether the window counted."""
        cfg = self.cfg
        dt = cfg.window_s
        if gates and momentary < cfg.abs_gate_dbfs:
            return False                                         # silence: hold
        if self.estimate is None:
            self.estimate = momentary
            return True
        if gates and momentary < self.estimate - cfg.rel_gate_db:
            self.gated_for += dt
            if self.gated_for < cfg.pause_hold_s:
                return False                                     # a pause between songs: hold
        else:
            self.gated_for = 0.0
        if momentary > self.estimate:
            # Rising: energy average (Leq-like), fast, so the boost backs off quickly.
            a = 1 - np.exp(-dt / cfg.attack_s)
            self.estimate = 10 * np.log10((1 - a) * 10 ** (self.estimate / 10) + a * 10 ** (momentary / 10))
        else:
            # Falling: in dB, with `speed` as the time constant (a 12 dB drop settles
            # to within 1 dB in ~2.5 x speed; in the power domain it would take ~4x).
            a = 1 - np.exp(-dt / cfg.speed_s)
            self.estimate += a * (momentary - self.estimate)
        return True


class Deadband:
    """The level the compensation uses. It only moves when the tracked estimate
    pushes more than `deadband_db` past it (backlash: no steps, the music's
    swells never reach it); inside the band it drifts towards the estimate
    with a `drift_s` time constant, so after a real change it lands exactly
    on the new level. Called once per tracker window."""

    def __init__(self, cfg: LoudnessConfig):
        self.cfg = cfg
        self.held: float | None = None

    def update(self, estimate: float | None) -> float | None:
        if estimate is None:
            return self.held
        if self.held is None:
            self.held = estimate
        elif estimate > self.held + self.cfg.deadband_db:
            self.held = estimate - self.cfg.deadband_db
        elif estimate < self.held - self.cfg.deadband_db:
            self.held = estimate + self.cfg.deadband_db
        else:
            self.held += (1 - np.exp(-self.cfg.window_s / self.cfg.drift_s)) * (estimate - self.held)
        return self.held


# ---------------------------------------------------------------------------
# Re-check: has the gain after the plugin changed since calibration?

RECHECK_BANDS = np.array([63.0 * 2 ** (k / 3) for k in range(22)])   # third octaves, 63 Hz - 8 kHz


def transfer_bands_db(output: np.ndarray, mic: np.ndarray, fs: float, min_coherence: float = 0.3) -> np.ndarray:
    """Output-to-mic gain per third octave (dB, NaN where the program didn't
    excite the band or the mic didn't hear it coherently)."""
    est = dualfft.transfer_function(output, mic, fs)
    gate = dualfft.excitation_gate(est) > 0
    out = np.full(len(RECHECK_BANDS), np.nan)
    for i, fc in enumerate(RECHECK_BANDS):
        sel = (est.freqs >= fc * 2 ** (-1 / 6)) & (est.freqs < fc * 2 ** (1 / 6)) & gate
        if np.count_nonzero(sel) < 3:
            continue
        if np.mean(est.coherence[sel]) < min_coherence:
            continue
        # |H1|^2 averaged over a band reads high by the incoherent power / K
        # (K = segments averaged); remove that before averaging.
        coherent = np.abs(est.H[sel]) ** 2 * est.gxx[sel]
        incoherent = np.maximum(est.gyy[sel] - coherent, 0.0)
        h2 = np.maximum(coherent - incoherent / est.n_segments, 0.0) / est.gxx[sel]
        if np.mean(h2) <= 0:
            continue
        out[i] = 10 * np.log10(np.mean(h2))
    return out


def recheck_gain_change(calibration_bands: np.ndarray, now_bands: np.ndarray, min_bands: int = 6) -> float | None:
    """Median per-band change (dB) between calibration and now, or None if too few bands compare."""
    both = np.isfinite(calibration_bands) & np.isfinite(now_bands)
    if np.count_nonzero(both) < min_bands:
        return None
    return float(np.median(now_bands[both] - calibration_bands[both]))


def recalibrated(cal: Calibration, gain_change_db: float) -> Calibration:
    """The gain after the plugin went up by gain_change_db: the same output level now gives that much more SPL."""
    return Calibration(output_dbfs=cal.output_dbfs - gain_change_db, spl=cal.spl,
                       mic_dbfs=cal.mic_dbfs)
