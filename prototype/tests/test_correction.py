import numpy as np
import pytest
import scipy.signal as ss

from roomeq import averaging, capture, correction, filters, roomsim, spectrum, sweep, targets
from roomeq.correction import CorrectionConfig, FitProblem
from roomeq.filters import Band

FS = 48000
CFG = CorrectionConfig()
GRID = spectrum.log_freq_grid(20, 20000, CFG.points_per_octave)
SWEEP_CFG = sweep.SweepConfig(duration=5.0)
PLAY = sweep.build_playback(SWEEP_CFG)
LOADIN = roomsim.NoiseSpec(pink_dbfs=-50.0, bursts=1, burst_dbfs=-25.0)


def problem(desired, lo=30.0, hi=18000.0, upper=None, max_cut=12.0, max_boost=3.0):
    inside = (GRID >= lo) & (GRID <= hi)
    up = np.where(inside, max_boost, 0.0) if upper is None else upper
    lower = np.full(len(GRID), -max_cut)
    return FitProblem(freqs=GRID, fs=FS, desired=np.where(inside, np.clip(desired, lower, up), 0.0),
                      weight=np.where(inside, 1.0, CFG.outside_weight), upper=up, lower=lower,
                      f_lo=lo, f_hi=hi, max_cut=max_cut, max_boost=max_boost, cfg=CFG)


def check_widths(bands):
    for b in bands:
        if b.kind != filters.BELL:
            continue
        octaves = filters.bandwidth_for_q(b.q)
        need = CFG.boost_min_octaves if b.gain_db > 0 else correction._min_cut_octaves(b.freq, CFG)
        assert need - 1e-6 <= octaves <= CFG.max_octaves + 1e-6, b


def test_recovers_a_reachable_correction():
    truth = [Band(filters.BELL, 150, -6, 1.0), Band(filters.BELL, 3000, 2, 1.2), Band(filters.HIGH_SHELF, 8000, -3)]
    prob = problem(filters.response_db(truth, GRID, FS))
    bands = correction.fit_bands(prob)
    fit = filters.response_db(bands, GRID, FS)
    inside = (GRID >= 30) & (GRID <= 18000)
    assert np.sqrt(np.mean((fit - prob.desired)[inside] ** 2)) < 0.25
    assert len(bands) <= 5


def test_limits_and_widths_hold_on_a_hostile_curve():
    rng = np.random.default_rng(5)
    ripple = spectrum.to_db(spectrum.smooth_power(np.arange(4097) * 6.0, 10 ** (rng.normal(0, 0.4, 4097)), 6, GRID))
    desired = (filters.band_db(Band(filters.BELL, 2000, 10, 8), GRID, FS)       # narrow +10 dB: can't follow
               + filters.band_db(Band(filters.BELL, 100, -20, 0.8), GRID, FS)    # deep and wide
               + ripple)
    prob = problem(desired)
    bands = correction.fit_bands(prob)
    fit = filters.response_db(bands, GRID, FS)
    assert len(bands) <= CFG.max_bands
    assert np.max(fit) <= 3.0 + 0.2
    assert np.min(fit) >= -12.0 - 0.2
    check_widths(bands)


def test_never_boosts_into_a_null():
    inside = (GRID >= 30) & (GRID <= 18000)
    null = (GRID > 800) & (GRID < 1300)
    upper = np.where(inside & ~null, 3.0, 0.0)
    desired = filters.band_db(Band(filters.BELL, 1000, 6, 0.7), GRID, FS)       # wants +6 dB right across the null
    bands = correction.fit_bands(problem(desired, upper=upper))
    fit = filters.response_db(bands, GRID, FS)
    assert np.max(fit[null]) <= 0.25
    assert np.max(fit) > 1.0            # still boosts either side of it


def test_detect_nulls_from_dips_and_disagreement():
    trend = np.zeros(len(GRID))
    fine = trend.copy()
    fine[(GRID > 950) & (GRID < 1050)] = -9.0                                  # deep dip in the average
    positions = [np.zeros(len(GRID)), np.zeros(len(GRID))]
    positions[1][(GRID > 60) & (GRID < 80)] = 8.0                              # positions disagree
    null = correction.detect_nulls(GRID, fine, trend, positions, CFG)
    assert null[np.argmin(np.abs(GRID - 1000))] and null[np.argmin(np.abs(GRID - 70))]
    assert null[np.argmin(np.abs(GRID - 1000 * 2 ** (1 / 8)))]                 # widened by the margin
    assert not null[np.argmin(np.abs(GRID - 300))] and not null[np.argmin(np.abs(GRID - 5000))]


# ---------------------------------------------------------------------------
# In the simulated club

def measure(sim, positions, rng, prefilter=None):
    play = PLAY if prefilter is None else ss.sosfilt(prefilter, PLAY)
    return [capture.analyze_sweep_capture(f"P{p + 1}", [sim.play(play, p, LOADIN, rng) for _ in range(2)], SWEEP_CFG)
            for p in positions]


@pytest.fixture(scope="module")
def club(sim):
    caps = measure(sim, range(5), np.random.default_rng(11))
    return caps, averaging.summarize_session(caps)


@pytest.mark.parametrize("target", targets.PRESETS, ids=lambda t: t.name)
def test_remeasuring_through_the_correction_lands_on_target(sim, club, target):
    """Phase 2 'done when': re-measure after applying the correction."""
    caps, summary = club
    res = correction.design_correction(caps, summary, target, FS)
    after = averaging.summarize_session(measure(sim, range(5), np.random.default_rng(12), filters.sos(res.bands, FS)))

    def third_octave(s):
        return spectrum.to_db(spectrum.smooth_power(s.freqs, s.power, 3, res.grid, s.weight))

    # Judge the corrected range: inside the fit range, outside nulls, and where the
    # target is reachable within the boost limit (a PA rolling off at the very top
    # stays short of a flat target by design).
    reachable = res.target_db - res.average_db <= CFG.max_boost_db + 1.0
    judge = (res.grid >= res.fit_range[0]) & (res.grid <= res.fit_range[1]) & ~res.null_mask & reachable
    assert np.count_nonzero(judge) > 0.8 * np.count_nonzero(res.grid >= 150)
    errors = []
    for s in (summary, after):
        level = third_octave(s)
        errors.append((level - targets.anchor_offset_db(res.grid, level, target) - target.db(res.grid))[judge])
    before, after_err = errors
    assert np.max(np.abs(after_err)) < 3.0
    assert np.sqrt(np.mean(after_err ** 2)) < 0.5 * np.sqrt(np.mean(before ** 2))


def test_room_correction_respects_limits(club):
    caps, summary = club
    res = correction.design_correction(caps, summary, targets.HOUSE, FS)    # wants +4 dB where the PA rolls off
    below = res.grid < res.fit_range[0]
    assert res.fit_range[0] > 30                                               # 55 Hz 4th-order PA high-pass
    assert np.max(res.correction_db[below]) <= 0.1                             # nothing boosted below the roll-off
    assert np.max(res.correction_db[res.null_mask]) <= 0.25
    assert np.max(res.correction_db) <= CFG.max_boost_db + 0.1
    assert np.min(res.correction_db) >= -CFG.max_cut_db - 0.1
    assert len(res.bands) <= CFG.max_bands
    check_widths(res.fitted)


def test_quick_mode_caps_the_applied_correction(club):
    caps, _ = club
    one = averaging.summarize_session(caps[:1])
    res = correction.design_correction(caps[:1], one, targets.FLAT, FS)
    assert res.strength == 0.5
    assert np.max(np.abs(res.correction_db)) <= 3.0 + 0.1
    assert np.max(res.correction_db) <= 0.5 * CFG.max_boost_db + 0.1


def test_crossover_notch_is_treated_as_a_null(sim):
    """A deep, narrow notch in the PA itself shows at every position: cut around it, never fill it."""
    pa = roomsim.PASpec(peq=roomsim.PASpec().peq + ((1400.0, -18.0, 5.0),))
    notched = roomsim.SimulatedRoom(pa=pa)
    caps = measure(notched, range(3), np.random.default_rng(13))
    res = correction.design_correction(caps, averaging.summarize_session(caps), targets.FLAT, FS)
    at = np.argmin(np.abs(res.grid - 1400))
    assert res.null_mask[at]
    assert res.correction_db[at] <= 0.25
