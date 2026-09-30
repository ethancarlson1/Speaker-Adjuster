import numpy as np
import pytest
import scipy.signal as ss

from roomeq import filters, iso226, loudness, roomsim
from roomeq.loudness import Calibration, LevelTracker, LoudnessConfig

CFG = LoudnessConfig()
FAST = LoudnessConfig(speed_s=5.0)      # the fastest Speed setting


# ---------------------------------------------------------------------------
# ISO 226 and C-weighting

# ISO 226:2003 contours at the standard's 29 frequencies (20 Hz-12.5 kHz), in dB
# SPL: the standard's formula with its Table 1 parameters, to two decimals.
# They agree with the widely used public implementations of the standard. If
# these change, the contour math changed: update them only on purpose.
ISO226_40_PHON = [99.85, 93.94, 88.17, 82.63, 77.78, 73.08, 68.48, 64.37, 60.59, 56.70, 53.41, 50.40, 47.58, 44.98,
                  43.05, 41.34, 40.06, 40.01, 41.82, 42.51, 39.23, 36.51, 35.61, 36.65, 40.01, 45.83, 51.80, 54.28,
                  51.49]
ISO226_80_PHON = [118.99, 114.23, 109.65, 105.34, 101.72, 98.36, 95.17, 92.48, 90.09, 87.82, 85.92, 84.31, 82.89,
                  81.68, 80.86, 80.17, 79.67, 80.01, 82.48, 83.74, 80.59, 77.88, 77.07, 78.31, 81.62, 86.81, 91.41,
                  91.74, 85.41]


def test_iso226_matches_the_standards_contours():
    assert list(iso226.FREQS) == [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                  1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500]
    assert iso226.contour_at_table(40) == pytest.approx(ISO226_40_PHON, abs=0.005)
    assert iso226.contour_at_table(80) == pytest.approx(ISO226_80_PHON, abs=0.005)
    # Between the table frequencies: linear in log frequency; held outside.
    mid = float(iso226.contour(np.array([np.sqrt(100 * 125)]), 40)[0])
    assert mid == pytest.approx((64.37 + 60.59) / 2, abs=0.01)
    assert float(iso226.contour(np.array([10.0]), 40)[0]) == pytest.approx(99.85, abs=0.005)
    assert float(iso226.contour(np.array([16000.0]), 40)[0]) == pytest.approx(51.49, abs=0.005)


def test_iso226_properties():
    def at(f, phon):
        return float(iso226.contour(np.array([f]), phon)[0])
    for phon in (20, 40, 60, 80, 90):
        assert at(1000, phon) == pytest.approx(phon, abs=0.02)       # 1 kHz defines the phon
        assert iso226.relative_contour(np.array([1000.0]), phon)[0] == pytest.approx(0.0, abs=1e-12)
    # Contours flatten as level rises: at low frequencies, loudness grows faster than level.
    assert at(50, 90) - at(50, 60) < 30 - 5
    assert at(20, 80) - at(20, 40) < at(1000, 80) - at(1000, 40)


@pytest.mark.parametrize("fs", [44100, 48000, 96000])
def test_c_weighting_within_iec_61672_class_1(fs):
    # (Hz, nominal dB, tolerance -, tolerance +)
    table = [(31.5, -3.0, 1.5, 1.5), (63, -0.8, 1.0, 1.0), (125, -0.2, 1.0, 1.0), (250, 0.0, 1.0, 1.0),
             (1000, 0.0, 0.7, 0.7), (2000, -0.2, 1.0, 1.0), (4000, -0.8, 1.0, 1.0), (8000, -3.0, 2.5, 1.5)]
    _, h = ss.sosfreqz(loudness.c_weighting_sos(fs), worN=[t[0] for t in table], fs=fs)
    for (f, nominal, lo, hi), db in zip(table, 20 * np.log10(np.abs(h))):
        assert nominal - lo <= db <= nominal + hi, f


@pytest.mark.parametrize("fs", [44100, 48000, 96000])
def test_a_weighting_within_iec_61672_class_1(fs):
    # (Hz, nominal dB, tolerance -, tolerance +)
    table = [(31.5, -39.4, 1.5, 1.5), (63, -26.2, 1.0, 1.0), (125, -16.1, 1.0, 1.0), (250, -8.6, 1.0, 1.0),
             (500, -3.2, 1.0, 1.0), (1000, 0.0, 0.7, 0.7), (2000, 1.2, 1.0, 1.0), (4000, 1.0, 1.0, 1.0),
             (8000, -1.1, 2.5, 1.5)]
    _, h = ss.sosfreqz(loudness.a_weighting_sos(fs), worN=[t[0] for t in table], fs=fs)
    for (f, nominal, lo, hi), db in zip(table, 20 * np.log10(np.abs(h))):
        assert nominal - lo <= db <= nominal + hi, f


# ---------------------------------------------------------------------------
# Compensation

def test_no_compensation_at_or_above_reference():
    f = np.geomspace(20, 20000, 50)
    assert np.all(loudness.compensation_target(f, 95.0, 0.0) == 0)
    assert np.all(loudness.compensation_target(f, 95.0, 5.0) == 0)
    plan = loudness.plan_shelves(95.0)
    assert loudness.shelf_gains(plan, 0.0, CFG) == (0.0, 0.0)
    assert loudness.shelf_gains(plan, 2.0, CFG) == (0.0, 0.0)


def test_compensation_boosts_the_ends_and_scales_with_the_drop():
    f = np.array([31.5, 100.0, 1000.0, 12500.0])
    d10 = loudness.compensation_target(f, 95.0, -10.0)
    d20 = loudness.compensation_target(f, 95.0, -20.0)
    assert d10[0] > d10[1] > 1.0 and abs(d10[2]) < 1e-9 and d10[3] > 1.0
    assert d20 == pytest.approx(2 * d10, rel=0.15)
    # It's the contours' change in shape: at 85 of 95, the 40-phon-style numbers from the standard.
    expected = (iso226.relative_contour(f, 85.0) - iso226.relative_contour(f, 95.0))
    assert d10 == pytest.approx(expected, abs=1e-12)


def test_the_level_change_sets_the_amount_and_the_reference_only_picks_the_region():
    # The same 8 dB drop asks for nearly the same tonal change from any reference in the usual range...
    f = np.array([31.5, 12500.0])
    drops = {ref: loudness.compensation_target(f, ref, -8.0) for ref in (80.0, 90.0, 95.0, 100.0)}
    for ref, d in drops.items():
        assert d == pytest.approx(drops[95.0], rel=0.1), ref
    # ...while twice the drop asks for about twice as much.
    assert loudness.compensation_target(f, 95.0, -16.0) == pytest.approx(2 * drops[95.0], rel=0.15)


def test_fitted_shelves_follow_the_contour_difference():
    plan = loudness.plan_shelves(95.0)
    grid = np.geomspace(30, 16000, 200)
    for drop in (5, 10, 15, 20):
        low = float(np.interp(drop, plan.deltas, plan.low_gain))
        high = float(np.interp(drop, plan.deltas, plan.high_gain))
        fit = filters.response_db([filters.Band(filters.LOW_SHELF, plan.low_freq, low, plan.low_q),
                                   filters.Band(filters.HIGH_SHELF, plan.high_freq, high, plan.high_q)], grid, 48000)
        err = fit - loudness.compensation_target(grid, 95.0, -drop)
        assert np.max(np.abs(err)) < 0.9, drop
    assert np.all(np.diff(plan.low_gain) >= 0) and np.all(np.diff(plan.high_gain) >= 0)


def test_limits_amount_and_ceiling():
    plan = loudness.plan_shelves(95.0)
    low, high = loudness.shelf_gains(plan, -30.0, CFG)                 # 30 dB down wants ~16 / 6 dB
    assert (low, high) == (CFG.max_low_db, CFG.max_high_db)
    wild = LoudnessConfig(max_low_db=20.0, max_high_db=20.0)
    assert loudness.shelf_gains(plan, -40.0, wild)[0] == CFG.low_ceiling_db   # the ceiling can't be raised
    half = LoudnessConfig(amount=0.5)
    full = loudness.shelf_gains(plan, -7.0, CFG)
    assert loudness.shelf_gains(plan, -7.0, half) == pytest.approx((full[0] / 2, full[1] / 2))


def test_low_boost_shrinks_as_the_pa_low_end_nears_the_shelf():
    plan = loudness.plan_shelves(95.0)
    shelf = plan.low_freq
    assert loudness.low_boost_allowance(0.0, shelf) == 1.0                     # not measured
    assert loudness.low_boost_allowance(shelf / 4, shelf) == 1.0               # subs: well below
    assert loudness.low_boost_allowance(shelf / 2, shelf) == pytest.approx(1.0)
    assert loudness.low_boost_allowance(shelf / np.sqrt(2), shelf) == pytest.approx(0.5)
    assert loudness.low_boost_allowance(shelf, shelf) == 0.0
    assert loudness.low_boost_allowance(2 * shelf, shelf) == 0.0

    deep = loudness.shelf_gains(plan, -30.0, LoudnessConfig(lf_limit_hz=35.0))
    small_top = loudness.shelf_gains(plan, -30.0, LoudnessConfig(lf_limit_hz=shelf / np.sqrt(2)))
    no_lows = loudness.shelf_gains(plan, -30.0, LoudnessConfig(lf_limit_hz=shelf * 1.2))
    assert deep == (CFG.max_low_db, CFG.max_high_db)
    assert small_top == pytest.approx((CFG.max_low_db / 2, CFG.max_high_db))
    assert no_lows == (0.0, CFG.max_high_db)                                   # the high shelf is untouched
    # Only the ceiling moves: a small boost that fits under it is left alone.
    gentle = loudness.shelf_gains(plan, -3.0, CFG)
    assert loudness.shelf_gains(plan, -3.0, LoudnessConfig(lf_limit_hz=shelf / np.sqrt(2))) == gentle


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
    for cfg in (CFG, FAST):
        tr = LevelTracker(48000, cfg)
        assert run(tr, pink(20, -20.0))[-1] == pytest.approx(-20.0, abs=0.1)
        assert not tr.jumping


def test_pauses_hold_and_long_drops_are_followed():
    fs = 48000
    tr = LevelTracker(fs, FAST)
    run(tr, pink(10, -20.0))
    run(tr, 1e-6 * np.random.default_rng(2).standard_normal(6 * fs))             # silence between songs
    held = tr.estimate
    assert held == pytest.approx(-20.0, abs=0.4)
    quiet = run(tr, pink(6, -45.0, seed=3))                                        # 25 dB lower: a pause at first
    assert quiet[int(4 * fs / 512)] == pytest.approx(held, abs=1e-9)
    run(tr, pink(40, -45.0, seed=4))                                               # ...but it lasts: follow it
    assert tr.estimate == pytest.approx(-45.0, abs=1.0)


def first_within(est, target, db, fs=48000, block=512):
    near = np.abs(est - target) < db
    return np.argmax(near) * block / fs if near.any() else np.inf


def test_a_songs_dynamics_barely_move_it():
    # 4 s verses and choruses 6 dB apart, for 90 s: the long average sits between them.
    fs = 48000
    song = np.concatenate([pink(4, -20.0 if k % 2 else -14.0, seed=20 + k) for k in range(24)])
    tr = LevelTracker(fs, CFG)
    run(tr, pink(20, -16.5, seed=19))
    est = run(tr, song)
    assert np.ptp(est[int(30 * fs / 512):]) < 1.0
    assert not tr.jumping
    fast = LevelTracker(fs, FAST)                                                   # 5 s follows them more
    run(fast, pink(20, -16.5, seed=19))
    assert np.ptp(run(fast, song)[int(30 * fs / 512):]) > 2.0


def test_louder_and_quieter_follow_the_speed():
    fs = 48000
    tr = LevelTracker(fs, CFG)
    run(tr, pink(20, -20.0))
    up = run(tr, pink(60, -16.0, seed=5))                                           # 4 dB up: under the jump guard
    assert not tr.jumping
    assert 20.0 < first_within(up, -16.0, 1.0) < 50.0
    down = run(tr, pink(90, -22.0, seed=6))                                         # 6 dB down, in dB over 30 s
    assert 40.0 < first_within(down, -22.0, 1.0) < 70.0
    fast = LevelTracker(fs, FAST)
    run(fast, pink(20, -16.0))
    down = run(fast, pink(40, -22.0, seed=6))
    assert 6.0 < first_within(down, -22.0, 1.0) < 14.0                              # 5 s: as quick as before


def test_a_big_jump_up_is_followed_within_seconds():
    # A loud song after a ballad: the boost for the quiet level mustn't sit on it.
    fs = 48000
    tr = LevelTracker(fs, CFG)
    run(tr, pink(60, -30.0))
    up = run(tr, pink(40, -18.0, seed=5))                                           # 12 dB up
    assert first_within(up, -18.0, 3.0) < 5.0
    assert first_within(up, -18.0, 1.0) < 40.0
    assert not tr.jumping                                                           # back on the long average
    # A loud moment inside a song (8 dB up for 2 s) doesn't trip it.
    hit = LevelTracker(fs, CFG)
    run(hit, pink(30, -30.0))
    est = run(hit, np.concatenate([pink(2, -22.0, seed=7), pink(10, -30.0, seed=8)]))
    assert np.max(est) < -28.5          # the long average moves ~1.3 dB (inside the deadband); tripped, it'd reach ~-22


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


# ---------------------------------------------------------------------------
# Deadband and re-check

def test_deadband_ignores_the_music_but_lands_on_a_real_change():
    fs, block = 48000, 512
    tr, db = LevelTracker(fs, FAST), loudness.Deadband(FAST)
    # 60 s whose level swells +-2.5 dB every 8 s, then a 6 dB pull-down.
    t = np.arange(60 * fs) / fs
    swell = pink(60, -20.0, seed=11) * 10 ** (2.5 * np.sin(2 * np.pi * t / 8.0) / 20)
    held, estimates = [], []
    for x in (swell, pink(90, -26.0, seed=12)):
        for i in range(0, len(x) - block + 1, block):
            tr.process(x[i:i + block])
            if tr.count < block:                  # a window just completed
                held.append(db.update(tr.estimate))
                estimates.append(tr.estimate)
    held, estimates = np.array(held, dtype=float), np.array(estimates, dtype=float)
    minute = slice(int(10 / CFG.window_s), int(60 / CFG.window_s))
    assert np.ptp(estimates[minute]) > 1.0                    # the tracker follows the swells...
    assert np.ptp(held[minute]) < 0.5                         # ...the EQ barely moves
    after = held[int(60 / CFG.window_s):]
    assert np.all(np.diff(after) <= 1e-9)                    # it follows the pull-down smoothly (no steps back)
    assert held[-1] == pytest.approx(-26.0, abs=0.5)         # and lands on it


def test_recheck_finds_an_amp_change_through_the_crowd(sim):
    fs = 48000
    rng = np.random.default_rng(21)
    # Calibration: pink noise at the mix position (quiet room).
    cal_noise = pink(10, -20.0, seed=13)
    cal_bands = loudness.transfer_bands_db(cal_noise, sim.play(cal_noise, 0, roomsim.NoiseSpec(pink_dbfs=-75.0), rng), fs)
    assert np.count_nonzero(np.isfinite(cal_bands)) >= 18

    program = roomsim.synthetic_program(12.0, fs, rng)
    clean = sim.play(program, 0, roomsim.NoiseSpec(pink_dbfs=None), rng)
    crowd_dbfs = loudness.c_weighted_level_dbfs(clean, fs) - 10.0            # crowd 10 dB under the program at the mic
    for amp_change in (0.0, 4.0, -6.0):
        heard = 10 ** (amp_change / 20) * clean + roomsim.make_noise(
            len(clean), fs, roomsim.NoiseSpec(pink_dbfs=None, babble_dbfs=crowd_dbfs + amp_change * 0), rng)
        change = loudness.recheck_gain_change(cal_bands, loudness.transfer_bands_db(program, heard, fs))
        assert change == pytest.approx(amp_change, abs=0.6), amp_change
    cal = Calibration(output_dbfs=-22.0, spl=95.0)
    assert loudness.recalibrated(cal, 4.0).spl_from_output(-22.0) == pytest.approx(99.0)


def test_recheck_refuses_without_enough_program():
    cal = np.zeros(len(loudness.RECHECK_BANDS))
    now = np.full(len(loudness.RECHECK_BANDS), np.nan)
    now[:3] = 1.0
    assert loudness.recheck_gain_change(cal, now) is None
