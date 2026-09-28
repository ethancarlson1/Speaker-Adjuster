import numpy as np
import pytest
import scipy.signal as ss

from roomeq import filters, iso226, loudness, roomsim
from roomeq.loudness import Calibration, LevelTracker, LoudnessConfig

CFG = LoudnessConfig()


# ---------------------------------------------------------------------------
# ISO 226 and C-weighting

def test_iso226_matches_published_values():
    def at(f, phon):
        return float(iso226.contour(np.array([f]), phon)[0])
    assert at(20, 40) == pytest.approx(99.85, abs=0.05)
    assert at(20, 60) == pytest.approx(109.51, abs=0.05)
    assert at(20, 80) == pytest.approx(118.99, abs=0.05)
    for phon in (20, 40, 60, 80, 90):
        assert at(1000, phon) == pytest.approx(phon, abs=0.05)       # 1 kHz defines the phon
    # Contours flatten as level rises: at low frequencies, loudness grows faster than level.
    assert at(50, 90) - at(50, 60) < 30 - 5


@pytest.mark.parametrize("fs", [44100, 48000, 96000])
def test_c_weighting_within_iec_61672_class_1(fs):
    # (Hz, nominal dB, tolerance -, tolerance +)
    table = [(31.5, -3.0, 1.5, 1.5), (63, -0.8, 1.0, 1.0), (125, -0.2, 1.0, 1.0), (250, 0.0, 1.0, 1.0),
             (1000, 0.0, 0.7, 0.7), (2000, -0.2, 1.0, 1.0), (4000, -0.8, 1.0, 1.0), (8000, -3.0, 2.5, 1.5)]
    _, h = ss.sosfreqz(loudness.c_weighting_sos(fs), worN=[t[0] for t in table], fs=fs)
    for (f, nominal, lo, hi), db in zip(table, 20 * np.log10(np.abs(h))):
        assert nominal - lo <= db <= nominal + hi, f


# ---------------------------------------------------------------------------
# Compensation

def test_no_compensation_at_or_above_reference():
    f = np.geomspace(20, 20000, 50)
    assert np.all(loudness.compensation_target(f, 95.0, 95.0) == 0)
    assert np.all(loudness.compensation_target(f, 100.0, 95.0) == 0)
    plan = loudness.plan_shelves(95.0)
    assert loudness.shelf_gains(plan, 95.0, CFG) == (0.0, 0.0)
    assert loudness.shelf_gains(plan, 97.0, CFG) == (0.0, 0.0)


def test_compensation_boosts_the_ends_and_scales_with_the_drop():
    f = np.array([31.5, 100.0, 1000.0, 12500.0])
    d10 = loudness.compensation_target(f, 85.0, 95.0)
    d20 = loudness.compensation_target(f, 75.0, 95.0)
    assert d10[0] > d10[1] > 1.0 and abs(d10[2]) < 1e-9 and d10[3] > 1.0
    assert d20 == pytest.approx(2 * d10, rel=0.15)


def test_fitted_shelves_follow_the_contour_difference():
    plan = loudness.plan_shelves(95.0)
    grid = np.geomspace(30, 16000, 200)
    for drop in (5, 10, 15, 20):
        low = float(np.interp(drop, plan.deltas, plan.low_gain))
        high = float(np.interp(drop, plan.deltas, plan.high_gain))
        fit = filters.response_db([filters.Band(filters.LOW_SHELF, plan.low_freq, low, plan.low_q),
                                   filters.Band(filters.HIGH_SHELF, plan.high_freq, high, plan.high_q)], grid, 48000)
        err = fit - loudness.compensation_target(grid, 95.0 - drop, 95.0)
        assert np.max(np.abs(err)) < 0.9, drop
    assert np.all(np.diff(plan.low_gain) >= 0) and np.all(np.diff(plan.high_gain) >= 0)


def test_limits_amount_and_ceiling():
    plan = loudness.plan_shelves(95.0)
    low, high = loudness.shelf_gains(plan, 65.0, CFG)                  # 30 dB down wants ~16 / 6 dB
    assert (low, high) == (CFG.max_low_db, CFG.max_high_db)
    wild = LoudnessConfig(max_low_db=20.0, max_high_db=20.0)
    assert loudness.shelf_gains(plan, 55.0, wild)[0] == CFG.low_ceiling_db   # the ceiling can't be raised
    half = LoudnessConfig(amount=0.5)
    full = loudness.shelf_gains(plan, 88.0, CFG)
    assert loudness.shelf_gains(plan, 88.0, half) == pytest.approx((full[0] / 2, full[1] / 2))


def test_tracking_highpass_rises_half_an_octave_at_full_boost():
    at_rest = loudness.tracking_highpass(45.0, 0.0, CFG)
    at_max = loudness.tracking_highpass(45.0, CFG.max_low_db, CFG)
    assert at_rest[0].freq == pytest.approx(45.0)
    assert at_max[0].freq == pytest.approx(45.0 * np.sqrt(2))
    # 24 dB/octave Butterworth: -3 dB at the corner, about -24 dB an octave below.
    db = filters.response_db(at_rest, np.array([45.0, 22.5]), 48000)
    assert db[0] == pytest.approx(-3.0, abs=0.1) and db[1] == pytest.approx(-24.0, abs=1.0)


# ---------------------------------------------------------------------------
# Calibration and tracking

def pink(seconds, level_dbfs_c, fs=48000, seed=1):
    x = roomsim.pink_noise(int(seconds * fs), fs, np.random.default_rng(seed))
    return x * 10 ** ((level_dbfs_c - loudness.c_weighted_level_dbfs(x, fs)) / 20)


def run(tracker, x, fs=48000, block=512, follow=None, follow_x=None):
    out = []
    for i in range(0, len(x) - block + 1, block):
        if follow is not None:
            follow.process(follow_x[i:i + block])
        tracker.process(x[i:i + block], follow)
        out.append(tracker.estimate)
    return np.array([np.nan if v is None else v for v in out])


def test_calibration_maps_output_level_to_spl():
    cal = Calibration(output_dbfs=-22.0, spl=88.0, mic_dbfs=-35.0)
    assert cal.spl_from_output(-32.0) == pytest.approx(78.0)
    assert cal.spl_from_mic(-30.0) == pytest.approx(93.0)
    assert Calibration(-22.0, 88.0).spl_from_mic(-30.0) is None


def test_steady_level_is_read_closely():
    # Rising faster than falling leans towards the peaks of the 400 ms level:
    # a fraction of a dB high, which errs towards less boost.
    tr = LevelTracker(48000, CFG)
    est = run(tr, pink(10, -20.0))
    assert -20.0 <= est[-1] < -19.6
    symmetric = LevelTracker(48000, LoudnessConfig(attack_s=CFG.speed_s))
    assert run(symmetric, pink(10, -20.0))[-1] == pytest.approx(-20.0, abs=0.05)


def test_pauses_hold_and_long_drops_are_followed():
    fs = 48000
    tr = LevelTracker(fs, CFG)
    run(tr, pink(10, -20.0))
    run(tr, 1e-6 * np.random.default_rng(2).standard_normal(6 * fs))             # silence between songs
    held = tr.estimate
    assert held == pytest.approx(-20.0, abs=0.4)
    quiet = run(tr, pink(6, -45.0, seed=3))                                        # 25 dB lower: a pause at first
    assert quiet[int(4 * fs / 512)] == pytest.approx(held, abs=1e-9)
    run(tr, pink(40, -45.0, seed=4))                                               # ...but it lasts: follow it
    assert tr.estimate == pytest.approx(-45.0, abs=1.0)


def test_rises_fast_falls_slowly():
    fs, block = 48000, 512
    tr = LevelTracker(fs, CFG)
    run(tr, pink(10, -20.0))
    up = run(tr, pink(6, -14.0, seed=5))
    first_within_1db = np.argmax(np.abs(up - (-14.0)) < 1.0) * block / fs
    assert first_within_1db < 2.5                                                   # boost backs off quickly
    down = run(tr, pink(40, -26.0, seed=6))
    first_within_1db = np.argmax(np.abs(down - (-26.0)) < 1.0) * block / fs
    assert 8.0 < first_within_1db < 25.0                                            # and grows back slowly


def test_mic_tracking_ignores_the_crowd_in_pauses():
    fs = 48000
    rng = np.random.default_rng(7)
    program = np.concatenate([pink(10, -20.0), np.zeros(6 * fs), pink(10, -20.0, seed=8)])
    crowd = roomsim.pink_noise(len(program), fs, rng) * 10 ** (-50 / 20)       # 10 dB under the program at the mic
    mic = 0.1 * program + crowd                                                  # mic hears program 20 dB down
    out_tr, mic_tr = LevelTracker(fs, CFG), LevelTracker(fs, CFG)
    est = run(mic_tr, mic, follow=out_tr, follow_x=program)
    in_pause = est[int(12 * fs / 512):int(15 * fs / 512)]
    program_at_mic = loudness.c_weighted_level_dbfs(0.1 * program[:10 * fs], fs)
    assert np.allclose(in_pause, in_pause[0])                                    # held through the pause
    assert 0.0 < est[-1] - program_at_mic < 1.5                                  # crowd reads slightly high: less boost
