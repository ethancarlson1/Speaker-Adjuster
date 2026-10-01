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


# ---------------------------------------------------------------------------
# System summary

from roomeq import averaging, correction, spectrum, targets  # noqa: E402


def session(sim, positions, noise=None, cfg=correction.CorrectionConfig(), seed=3):
    rng = np.random.default_rng(seed)
    caps = []
    for i, p in enumerate(positions):
        spec = (noise or {}).get(i, roomsim.NoiseSpec())
        caps.append(capture.analyze_sweep_capture(f"P{i + 1}", [sim.play(PLAY, p, spec, rng) for _ in range(2)], CFG))
    summary = averaging.summarize_session(caps)
    result = correction.design_correction(caps, summary, targets.FLAT, CFG.fs, cfg)
    return caps, summary, result, quality.system_summary(caps, summary, result, cfg)


def test_four_clean_positions(sim):
    caps, summary, result, s = session(sim, [0, 1, 2, 3])
    assert s.confidence == quality.HIGH and s.confidence_reasons == [] and s.positions == 4
    assert s.variation_db is not None and 0.3 < s.variation_db < 5.0 and s.coverage is not None
    assert s.usable == summary.usable and s.filters == len(result.bands) > 0
    assert s.largest_cut_db <= 0.0 <= s.largest_boost_db <= 3.0 + 1e-9
    assert s.before_db > s.after_db == result.rms_error_db
    assert s.issue_db is not None and abs(s.issue_db) > 1.0 and result.fit_range[0] <= s.issue_hz <= result.fit_range[1]
    kinds = [e.kind for e in s.explanations]
    assert "positions" not in kinds
    # The simulated PA rolls off at the bottom: that's where the fit stops, and it says so (not at the top: it never drops).
    low = [e for e in s.explanations if e.kind == "bandwidth"]
    assert len(low) == 1 and low[0].text.startswith(f"Not correcting below {spectrum.format_hz(result.fit_range[0])}")
    assert s.nulls_ignored == sum(1 for k in kinds if k in ("null", "disagreement"))


def test_two_positions_are_quick_mode_and_say_so(sim):
    _, _, result, s = session(sim, [0, 1])
    assert s.confidence == quality.MEDIUM and s.confidence_reasons[0].startswith("2 good positions")
    first = s.explanations[0]
    assert first.kind == "positions" and first.text.startswith("Only 2 good positions: the fit smooths to 1/3 octave")
    assert "75% strength, at most 6 dB. Measure 1 more" in first.text
    limits = [e for e in s.explanations if e.kind == "limit"]
    assert limits and all("(the quick-mode cap)" in e.text for e in limits if e.text.startswith("Boost"))


def test_the_limits_and_noise_explain_themselves(sim):
    cfg = correction.CorrectionConfig(max_cut_db=2.0)
    caps, _, _, s = session(sim, [0, 1, 2, 3], noise={3: roomsim.NoiseSpec(rumble_dbfs=-25.0)}, cfg=cfg)
    cuts = [e for e in s.explanations if e.kind == "limit" and e.text.startswith("Cut held to -2.0 dB")]
    assert cuts and "(Max cut)" in cuts[0].text
    noisy = [e for e in s.explanations if e.kind == "noise"]
    assert noisy and all("P4 left out of the average there (too noisy)" in e.text for e in noisy)
    assert {e.text.split(" Hz band")[0] for e in noisy} <= {"31.5", "63", "125"}
    assert s.confidence == quality.MEDIUM and any(r.startswith("P4: ") for r in s.confidence_reasons)


def test_the_users_range_is_not_explained_as_the_systems(sim):
    _, _, result, s = session(sim, [0, 1, 2, 3], cfg=correction.CorrectionConfig(range_hz=(100.0, 10000.0)))
    assert result.fit_range == (100.0, 10000.0) and not [e for e in s.explanations if e.kind == "bandwidth"]
