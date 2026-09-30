"""Measurement quality: the grading's numbers as ratings and a confidence."""
import numpy as np
import pytest

from roomeq import capture, grading, quality, roomsim, sweep

CFG = sweep.SweepConfig(duration=2.0)
PLAY = sweep.build_playback(CFG)


def sweeps(sim, positions, noise=roomsim.NoiseSpec(), seed=1):
    rng = np.random.default_rng(seed)
    return capture.analyze_sweep_capture("P", [sim.play(PLAY, p, noise, rng) for p in positions], CFG)


def test_ratings_follow_the_grading_limits():
    assert [quality.rate(v, quality.SNR_LIMITS_DB) for v in (35, 30, 25, 20, 15, 10, 5)] == \
        ["Excellent", "Excellent", "Good", "Good", "Fair", "Fair", "Poor"]
    assert [quality.rate(v, quality.REPEAT_LIMITS_DB, False) for v in (0.2, 0.5, 0.8, 1.0, 2.0, 3.0, 4.0)] == \
        ["Excellent", "Excellent", "Good", "Good", "Fair", "Fair", "Poor"]
    # Good and Fair start where the grading's pass and marginal do.
    g = grading.GradingConfig()
    assert quality.SNR_LIMITS_DB[1:] == (g.snr_pass_db, g.snr_marginal_db)
    assert quality.REPEAT_LIMITS_DB[1:] == (g.consistency_pass_db, g.consistency_marginal_db)


def test_clean_repeat_sweeps_are_high_confidence(sim):
    q = quality.measurement_quality(sweeps(sim, [0, 0]))
    assert q.snr in ("Good", "Excellent") and q.repeatability == "Excellent"
    assert q.coherence is None and q.coherence_rating is None           # sweeps have no coherence
    assert q.confidence == quality.HIGH and q.reasons == []
    lo, hi = q.usable
    assert 25.0 < lo < 60.0 and hi > 15000.0


def test_one_sweep_has_no_repeatability(sim):
    q = quality.measurement_quality(sweeps(sim, [0]))
    assert q.repeatability is None and q.repeat_db is None and q.confidence == quality.HIGH


def test_noise_and_movement_lower_the_confidence(sim):
    noisy = quality.measurement_quality(sweeps(sim, [0, 0], roomsim.NoiseSpec(pink_dbfs=-35.0)))
    assert noisy.snr in ("Fair", "Poor") and noisy.confidence in (quality.MEDIUM, quality.LOW)
    assert noisy.reasons[0].startswith("signal-to-noise ") and " dB at " in noisy.reasons[0]
    assert all(np.isnan(noisy.usable)) or noisy.usable[0] < noisy.usable[1]
    moved = quality.measurement_quality(sweeps(sim, [0, 3]))          # the mic moved between the repeats
    assert moved.repeatability == "Poor" and moved.confidence == quality.LOW
    assert any(r.startswith("repeatability poor") for r in moved.reasons)


def test_music_has_coherence_per_band(sim):
    rng = np.random.default_rng(5)
    x = roomsim.synthetic_program(12.0, CFG.fs, rng)
    clean = capture.analyze_program_capture("W", x, sim.play(x, 1, roomsim.NoiseSpec(), rng), CFG.fs)
    noisy = capture.analyze_program_capture("W", x, sim.play(x, 1, roomsim.NoiseSpec(babble_dbfs=-45.0), rng), CFG.fs)
    for c in (clean, noisy):
        graded = [b for b in c.grade.bands if not b.out_of_range]
        assert all(b.coherence is not None and 0.0 <= b.coherence <= 1.0 for b in graded)
        assert all(b.spread_db is None for b in graded)
    qc, qn = quality.measurement_quality(clean), quality.measurement_quality(noisy)
    assert qc.coherence > qn.coherence and qc.repeatability is None
    assert qn.coherence_rating in ("Fair", "Poor") and qn.confidence == quality.LOW
    # Re-grading for another band keeps the coherence.
    capture.regrade_capture(noisy, grading.GradingConfig(passband=(40.0, 100.0)))
    assert quality.measurement_quality(noisy, (40.0, 100.0)).coherence == pytest.approx(qn.coherence)


def test_a_sub_is_judged_on_its_own_band():
    sub = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=30.0, hp_order=4, lp_hz=100.0, lp_order=4, peq=()))
    rng = np.random.default_rng(7)
    c = capture.analyze_sweep_capture("S", [sub.play(PLAY, 0, roomsim.NoiseSpec(), rng) for _ in range(2)], CFG,
                                      capture.AnalysisConfig(grading=grading.GradingConfig(passband=(40.0, 100.0))))
    lo, hi = quality.measurement_quality(c, (40.0, 100.0)).usable
    assert 20.0 <= lo < 40.0 and 100.0 < hi < 250.0
