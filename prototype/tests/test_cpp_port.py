"""Cross-check: the C++ analysis core must reproduce the Python prototype.

Runs the built `roomeq_cli` on simulated recordings and compares every
number the plugin shows: delays, band SNR / level / spread, grades, reason
text, level offsets, smoothed curves, usable range and target level.

Set ROOMEQ_CLI to the built binary (e.g. build/roomeq_cli) to run these;
they are skipped otherwise. CI always sets it.
"""

import json
import os
import subprocess
from pathlib import Path

import numpy as np
import pytest

from roomeq import alignment, averaging, capture, loudness, quality, roomsim, sweep
from roomeq.spectrum import log_freq_grid

CLI = os.environ.get("ROOMEQ_CLI")
pytestmark = pytest.mark.skipif(not CLI, reason="set ROOMEQ_CLI to the built roomeq_cli to cross-check the C++ port")

CFG = sweep.SweepConfig(duration=2.0)
TOL_DB = 1e-6


def run_cli(*args):
    out = subprocess.run([CLI, *map(str, args)], check=True, capture_output=True, text=True)
    return json.loads(out.stdout) if out.stdout.strip() else None


def save(tmp_path: Path, name: str, data: np.ndarray) -> Path:
    path = tmp_path / f"{name}.f64"
    np.asarray(data, dtype="<f8").tofile(path)
    return path


def as_array(values):
    return np.array([np.nan if v is None else v for v in values], dtype=float)


def assert_curve(cpp, py, what):
    cpp = as_array(cpp)
    assert np.array_equal(np.isnan(cpp), np.isnan(py)), f"{what}: NaN pattern differs"
    ok = ~np.isnan(py)
    assert np.max(np.abs(cpp[ok] - py[ok]), initial=0.0) < TOL_DB, what


def assert_quality(cpp, py: capture.Capture, band=(250.0, 4000.0)):
    q = quality.measurement_quality(py, band)
    where = py.name
    for key in ("snr_db", "coherence", "repeat_db"):
        value = getattr(q, key)
        if value is None:
            assert cpp[key] is None, f"{where} {key}"
        else:
            assert cpp[key] == pytest.approx(value, abs=1e-9), f"{where} {key}"
    for key in ("snr_band", "snr", "coherence_band", "coherence_rating", "repeat_band", "repeatability", "confidence", "reasons"):
        assert cpp[key] == getattr(q, key), f"{where} {key}"
    assert as_array(cpp["usable"]) == pytest.approx(np.array(q.usable), abs=1e-9, nan_ok=True), where


def test_sweep_generation_matches(tmp_path):
    for fs, duration in ((44100, 2.0), (48000, 5.0), (96000, 10.0)):
        out = tmp_path / f"sweep_{fs}_{duration}.f64"
        run_cli("sweep", "--fs", fs, "--duration", duration, "--level", -12, "--out", out)
        cpp = np.fromfile(out, dtype="<f8")
        py = sweep.generate_sweep(sweep.SweepConfig(fs=fs, duration=duration))
        assert len(cpp) == len(py)
        assert np.max(np.abs(cpp - py)) < 1e-9     # sin() rounding at ~36k rad phase differs by ~1e-12


def test_session_matches_python(sim, tmp_path):
    rng = np.random.default_rng(31)
    play = sweep.build_playback(CFG)
    loadin = roomsim.NoiseSpec(pink_dbfs=-50.0, bursts=1, burst_dbfs=-20.0)
    scenario = [
        ("P1", 0, 2, loadin),
        ("P2", 1, 1, roomsim.NoiseSpec()),
        ("P3", 2, 2, roomsim.NoiseSpec(rumble_dbfs=-25.0)),      # graded redo below ~180 Hz
        ("P4", 3, 2, roomsim.NoiseSpec(pink_dbfs=-40.0)),        # noisy, excluded by the user
    ]
    args, py_caps = [], []
    for name, pos, repeats, noise in scenario:
        recs = [sim.play(play, pos, noise, rng) for _ in range(repeats)]
        files = ",".join(str(save(tmp_path, f"{name}_{i}", r)) for i, r in enumerate(recs))
        args += ["--position", f"{name}={files}"]
        py_caps.append(capture.analyze_sweep_capture(name, recs, CFG))

    x = roomsim.synthetic_program(12.0, CFG.fs, rng)
    y = sim.play(x, 4, roomsim.NoiseSpec(babble_dbfs=-45.0), rng)
    args += ["--program", f"W={save(tmp_path, 'ref', x)}:{save(tmp_path, 'mic', y)}", "--exclude", "P4"]
    py_caps.append(capture.analyze_program_capture("W", x, y, CFG.fs))
    py_caps[3].excluded = True

    result = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--smoothing", 6, *args)

    for cpp, py in zip(result["captures"], py_caps):
        name = py.name
        assert cpp["name"] == name and cpp["kind"] == py.kind and cpp["excluded"] == py.excluded
        assert np.allclose(cpp["delays_ms"], py.delays_ms, atol=1e-9), name
        arrival = alignment.estimate_arrival(py)
        assert cpp["arrival"]["ms"] == pytest.approx(arrival.ms, abs=1e-9), name
        assert cpp["arrival"]["confidence"] == arrival.confidence and cpp["arrival"]["reasons"] == arrival.reasons, name
        if py.low is None:
            assert cpp["low"] is None, name
        else:
            low = np.array([complex(*z) for z in cpp["low"]])
            assert np.max(np.abs(low - py.low)) < 1e-9 * np.max(np.abs(py.low)), name
        assert cpp["overall"] == py.grade.overall.label, name
        assert cpp["reasons"] == py.grade.reasons, name
        assert cpp["notes"] == py.grade.notes, name
        for cb, pb in zip(cpp["bands"], py.grade.bands):
            where = f"{name} {pb.name}"
            assert cb["out_of_range"] == pb.out_of_range, where
            assert cb["grade"] == (None if pb.grade is None else pb.grade.label), where
            assert abs(cb["snr_db"] - pb.snr_db) < TOL_DB, where
            assert abs(cb["level_db"] - pb.level_db) < TOL_DB, where
            for key, value in (("spread_db", pb.spread_db), ("excess_spread_db", pb.excess_spread_db)):
                if value is None:
                    assert cb[key] is None, where
                else:
                    assert abs(cb[key] - value) < TOL_DB, where
            if pb.coherence is None:
                assert cb["coherence"] is None, where
            else:
                assert cb["coherence"] == pytest.approx(pb.coherence, abs=1e-9), where
        assert_quality(cpp["quality"], py)

    # The scenario must exercise every grade path, or the comparison proves little.
    assert {c.grade.overall.label for c in py_caps} >= {"pass", "redo"}
    assert {quality.measurement_quality(c).confidence for c in py_caps} == {"high", "medium", "low"}

    py_sum = averaging.summarize_session(py_caps, 6, log_freq_grid(20, 20000, 48))
    s = result["summary"]
    assert s["n_good"] == py_sum.n_good
    assert s["smoothing_fraction"] == py_sum.policy.smoothing_fraction
    assert np.allclose(s["offsets_db"], py_sum.offsets_db, atol=TOL_DB)
    assert_curve(s["grid"], py_sum.grid, "grid")
    assert_curve(s["average_db"], py_sum.average_db, "average")
    for i, (cpp_curve, py_curve) in enumerate(zip(s["position_db"], py_sum.position_db)):
        assert_curve(cpp_curve, py_curve, f"position {i}")
    assert s["usable"] == pytest.approx(list(py_sum.usable), abs=1e-9)
    assert s["target_db"] == pytest.approx(py_sum.target_db, abs=TOL_DB)


def test_captures_across_two_clocks_match_python(sim, tmp_path):
    """Music and pink noise with the mic's clock off (an aggregate device): the
    measured drift, the correction, the grades and the notes all agree."""
    rng = np.random.default_rng(34)
    rec = sim.play(sweep.build_playback(CFG), 0, roomsim.NoiseSpec(), rng)
    music = roomsim.synthetic_program(20.0, CFG.fs, rng)
    pink = rng.standard_normal(20 * CFG.fs) * 0.05
    cases = [("M", music, sim.play(music, 2, roomsim.NoiseSpec(), rng, drift_ppm=18.0)),
             ("N", pink, sim.play(pink, 1, roomsim.NoiseSpec(), rng, drift_ppm=-31.0)),
             ("S", pink, sim.play(pink, 3, roomsim.NoiseSpec(), rng))]
    args = ["--position", f"P1={save(tmp_path, 'p1', rec)}"]
    py = []
    for name, x, y in cases:
        args += ["--program", f"{name}={save(tmp_path, name + 'x', x)}:{save(tmp_path, name + 'y', y)}"]
        py.append(capture.analyze_program_capture(name, x, y, CFG.fs))
    result = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--smoothing", 6, *args)

    assert result["captures"][0]["drift_ppm"] is None                  # sweeps don't measure it
    for cpp, p in zip(result["captures"][1:], py):
        assert cpp["drift_ppm"] == pytest.approx(p.drift_ppm, abs=1e-6), p.name
        assert np.allclose(cpp["delays_ms"], p.delays_ms, atol=1e-9), p.name
        assert cpp["overall"] == p.grade.overall.label, p.name
        assert cpp["reasons"] == p.grade.reasons and cpp["notes"] == p.grade.notes, p.name
        for cb, pb in zip(cpp["bands"], p.grade.bands):
            assert abs(cb["snr_db"] - pb.snr_db) < TOL_DB and abs(cb["level_db"] - pb.level_db) < TOL_DB, (p.name, pb.name)
    m, n, still = py
    assert m.drift_ppm == pytest.approx(18.0, abs=0.2) and n.drift_ppm == pytest.approx(-31.0, abs=0.2)
    assert still.drift_ppm == 0.0 and not any("clocks" in note for note in still.grade.notes)
    assert "output and mic clocks differ by 31.0 ppm (corrected)" in n.grade.notes


def test_quick_mode_and_all_excluded(sim, tmp_path):
    rng = np.random.default_rng(32)
    rec = sim.play(sweep.build_playback(CFG), 0, roomsim.NoiseSpec(), rng)
    path = save(tmp_path, "one", rec)
    one = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--smoothing", 6, "--position", f"P1={path}")
    assert one["summary"]["smoothing_fraction"] == 2 and one["summary"]["max_correction_db"] == 3.0
    none = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--position", f"P1={path}", "--exclude", "P1")
    assert none["summary"] is None


def test_tail_too_short_is_an_error(sim, tmp_path):
    cfg = sweep.SweepConfig(duration=2.0, tail=0.6)
    rec = sim.play(sweep.build_playback(cfg), 3, roomsim.NoiseSpec(), np.random.default_rng(33))
    with pytest.raises(ValueError):
        capture.analyze_sweep_capture("P", [rec], cfg)
    out = subprocess.run([CLI, "analyze", "--fs", "48000", "--duration", "2", "--tail", "0.6",
                          "--position", f"P={save(tmp_path, 'short', rec)}"], capture_output=True, text=True)
    assert out.returncode == 1 and "tail too short" in out.stderr


# ---------------------------------------------------------------------------
# Phase 2: the correction fit

FIT_TOL_DB = 0.05       # rounding can steer the optimiser differently per platform (locally ~2e-6 dB)


def _fit_session(sim, tmp_path, positions, seed):
    rng = np.random.default_rng(seed)
    play = sweep.build_playback(CFG)
    args, caps = [], []
    for pos, noise in positions:
        recs = [sim.play(play, pos, noise, rng) for _ in range(2)]
        files = ",".join(str(save(tmp_path, f"fit{pos}_{i}", r)) for i, r in enumerate(recs))
        args += ["--position", f"P{pos}={files}"]
        caps.append(capture.analyze_sweep_capture(f"P{pos}", recs, CFG))
    return args, caps


@pytest.mark.parametrize("target_name,positions,limits", [
    ("house", [(0, roomsim.NoiseSpec()), (1, roomsim.NoiseSpec()), (2, roomsim.NoiseSpec(rumble_dbfs=-25.0)),
               (3, roomsim.NoiseSpec())], {}),
    ("speech", [(0, roomsim.NoiseSpec()), (2, roomsim.NoiseSpec()), (4, roomsim.NoiseSpec())],
     {"max_cut_db": 9.0, "max_boost_db": 2.0, "range_hz": (60.0, 12000.0)}),
    ("flat", [(1, roomsim.NoiseSpec())], {}),                                            # quick mode
])
def test_correction_fit_matches_python(sim, tmp_path, target_name, positions, limits):
    from roomeq import correction, filters, targets

    args, caps = _fit_session(sim, tmp_path, positions, 41 + len(positions))
    cfg = correction.CorrectionConfig(**limits)
    extra = ["--target", target_name, "--max-cut", cfg.max_cut_db, "--max-boost", cfg.max_boost_db,
             "--range-lo", cfg.range_hz[0], "--range-hi", cfg.range_hz[1]]
    result = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--smoothing", 6, *args, *extra)
    cpp = result["correction"]

    target = {t.name.lower(): t for t in targets.PRESETS}[target_name]
    summary = averaging.summarize_session(caps, 6, log_freq_grid(20, 20000, 48))
    py = correction.design_correction(caps, summary, target, CFG.fs, cfg)

    # Everything before the optimiser is deterministic: exact to rounding.
    assert_curve(cpp["grid"], py.grid, "grid")
    assert_curve(cpp["average_db"], py.average_db, "average")
    assert_curve(cpp["target_db"], py.target_db, "target")
    assert_curve(cpp["desired_db"], py.desired_db, "desired")
    assert np.array_equal(np.array(cpp["null_mask"], dtype=bool), py.null_mask)
    assert cpp["fit_range"] == pytest.approx(list(py.fit_range), abs=1e-9)
    assert cpp["strength"] == py.strength

    # The fit itself: same bands, same curve.
    assert len(cpp["bands"]) == len(py.bands)
    for cb, pb in zip(cpp["bands"], py.bands):
        assert cb["kind"] == pb.kind
    assert np.max(np.abs(np.array(cpp["correction_db"]) - py.correction_db)) < FIT_TOL_DB
    cpp_bands = [filters.Band(b["kind"], b["freq"], b["gain_db"], b["q"]) for b in cpp["bands"]]
    assert np.max(np.abs(filters.response_db(cpp_bands, py.grid, CFG.fs) - py.correction_db)) < FIT_TOL_DB
    assert cpp["rms_error_db"] == pytest.approx(py.rms_error_db, abs=FIT_TOL_DB)


def test_sub_zone_matches_python(tmp_path):
    """A sub: graded, summarised and corrected against the 40-100 Hz reference band."""
    from roomeq import correction, grading, targets

    band = (40.0, 100.0)
    room = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=30.0, hp_order=4, lp_hz=100.0, lp_order=4,
                                                   peq=((55.0, 5.0, 2.0),), h2_db=-50.0, h3_db=-55.0))
    acfg = capture.AnalysisConfig(grading=grading.GradingConfig(passband=band))
    rng = np.random.default_rng(53)
    play = sweep.build_playback(CFG)
    args, caps = [], []
    for pos in range(3):
        recs = [room.play(play, pos, roomsim.NoiseSpec(pink_dbfs=-60.0), rng) for _ in range(2)]
        files = ",".join(str(save(tmp_path, f"sub{pos}_{i}", r)) for i, r in enumerate(recs))
        args += ["--position", f"S{pos}={files}"]
        caps.append(capture.analyze_sweep_capture(f"S{pos}", recs, CFG, acfg))
    cfg = correction.CorrectionConfig(ref_band=band, range_hz=(20.0, 150.0))
    result = run_cli("analyze", "--fs", CFG.fs, "--duration", CFG.duration, "--smoothing", 6, *args,
                     "--band-lo", band[0], "--band-hi", band[1], "--target", "flat",
                     "--range-lo", 20, "--range-hi", 150)

    for cpp, py in zip(result["captures"], caps):
        assert cpp["overall"] == py.grade.overall.label, py.name
        assert cpp["reasons"] == py.grade.reasons, py.name
        for cb, pb in zip(cpp["bands"], py.grade.bands):
            assert cb["out_of_range"] == pb.out_of_range, f"{py.name} {pb.name}"
            assert cb["grade"] == (None if pb.grade is None else pb.grade.label), f"{py.name} {pb.name}"
            assert abs(cb["level_db"] - pb.level_db) < TOL_DB, f"{py.name} {pb.name}"
        assert_quality(cpp["quality"], py, band)
    assert any(not b.out_of_range for b in caps[0].grade.bands) and caps[0].grade.bands[-1].out_of_range

    summary = averaging.summarize_session(caps, 6, log_freq_grid(20, 20000, 48), band=band)
    s = result["summary"]
    assert np.allclose(s["offsets_db"], summary.offsets_db, atol=TOL_DB)
    assert_curve(s["average_db"], summary.average_db, "average")
    assert s["usable"] == pytest.approx(list(summary.usable), abs=1e-9)
    assert summary.usable[1] < 200.0
    assert s["target_db"] == pytest.approx(summary.target_db, abs=TOL_DB)

    py = correction.design_correction(caps, summary, targets.FLAT, CFG.fs, cfg)
    cpp = result["correction"]
    assert_curve(cpp["target_db"], py.target_db, "target")
    assert_curve(cpp["desired_db"], py.desired_db, "desired")
    assert cpp["fit_range"] == pytest.approx(list(py.fit_range), abs=1e-9)
    assert len(cpp["bands"]) == len(py.bands) > 0
    assert np.max(np.abs(np.array(cpp["correction_db"]) - py.correction_db)) < FIT_TOL_DB


# ---------------------------------------------------------------------------
# Phase 3: loudness plan, level tracking, re-check bands

def test_loudness_plan_matches_python():
    from roomeq import loudness

    for ref, fs in ((95.0, 48000), (85.0, 44100)):
        cpp = run_cli("loudness-plan", "--ref", ref, "--fs", fs)
        py = loudness.plan_shelves(ref, fs)
        assert cpp["low_freq"] == pytest.approx(py.low_freq, rel=1e-9)
        assert cpp["low_q"] == pytest.approx(py.low_q, rel=1e-12)
        assert cpp["high_freq"] == pytest.approx(py.high_freq, rel=1e-9)
        assert np.max(np.abs(np.array(cpp["low_gain"]) - py.low_gain)) < 1e-6
        assert np.max(np.abs(np.array(cpp["high_gain"]) - py.high_gain)) < 1e-6
    # Gains for a level change, with the amount and the PA's low end limiting them.
    for delta, lf_limit, amount in ((-8.0, 0.0, 1.0), (-25.0, 90.0, 0.75), (-12.0, 60.0, 1.0), (3.0, 0.0, 1.0)):
        cpp = run_cli("loudness-plan", "--ref", 95.0, "--fs", 48000, "--delta", delta, "--lf-limit", lf_limit, "--amount", amount)
        plan = loudness.plan_shelves(95.0, 48000)
        cfg = loudness.LoudnessConfig(lf_limit_hz=lf_limit, amount=amount)
        assert cpp["max_low_boost"] == pytest.approx(loudness.max_low_boost(plan, cfg), abs=1e-9)
        assert cpp["gains"] == pytest.approx(list(loudness.shelf_gains(plan, delta, cfg)), abs=1e-6)


def test_level_tracker_and_deadband_match_python(tmp_path):
    from roomeq import loudness

    fs, block = 48000, 512
    rng = np.random.default_rng(51)
    # Program with a pause, a long quiet stretch and a jump back up (the big-jump
    # guard); the mic hears it 12 dB down plus a crowd.
    parts = [roomsim.synthetic_program(20.0, fs, rng, -18.0), np.zeros(6 * fs),
             roomsim.synthetic_program(40.0, fs, rng, -34.0), roomsim.synthetic_program(15.0, fs, rng, -18.0)]
    out = np.concatenate(parts).astype(np.float32).astype(float)       # the plugin tracks float samples
    mic = (0.25 * out + roomsim.make_noise(len(out), fs, roomsim.NoiseSpec(pink_dbfs=None, babble_dbfs=-50.0), rng))
    mic = mic.astype(np.float32).astype(float)
    paths = save(tmp_path, "out", out), save(tmp_path, "mic", mic)

    for speed in (loudness.LoudnessConfig().speed_s, 5.0):     # the default (30 s) and the fastest setting
        cpp = run_cli("track", "--fs", fs, "--in", paths[0], "--mic", paths[1], "--speed", speed)
        cfg = loudness.LoudnessConfig(speed_s=speed)
        out_tr, mic_tr, db = loudness.LevelTracker(fs, cfg), loudness.LevelTracker(fs, cfg), loudness.Deadband(cfg)
        est, held, mic_est, jumped = [], [], [], False
        for i in range(0, len(out) - block + 1, block):
            out_tr.process(out[i:i + block])
            mic_tr.process(mic[i:i + block], follow=out_tr)
            jumped = jumped or out_tr.jumping
            if out_tr.count < block:                           # a window completed in this block
                est.append(np.nan if out_tr.estimate is None else out_tr.estimate)
                h = db.update(out_tr.estimate)
                held.append(np.nan if h is None else h)
                mic_est.append(np.nan if mic_tr.estimate is None else mic_tr.estimate)
        assert jumped                                          # the push back up trips the big-jump guard
        assert len(cpp["estimates"]) == len(est) > 150
        assert_curve(cpp["estimates"], np.array(est), f"tracker ({speed:g} s)")
        assert_curve(cpp["held"], np.array(held), f"deadband ({speed:g} s)")
        assert_curve(cpp["mic"], np.array(mic_est), f"mic tracker ({speed:g} s)")


def test_transfer_bands_match_python(sim, tmp_path):
    from roomeq import loudness

    fs = 48000
    rng = np.random.default_rng(52)
    x = roomsim.synthetic_program(12.0, fs, rng)
    y = sim.play(x, 0, roomsim.NoiseSpec(pink_dbfs=None, babble_dbfs=-45.0), rng)
    cpp = run_cli("transfer-bands", "--fs", fs, "--out", save(tmp_path, "x", x), "--mic", save(tmp_path, "y", y))
    assert_curve(cpp["bands"], loudness.transfer_bands_db(x, y, fs), "re-check bands")


def test_arrivals_match_python(tmp_path):
    """Direct arrivals and their confidence, on synthetic impulse responses that
    exercise every path: clean, a louder later arrival, a direct sound under
    the threshold, and repeats that disagree."""
    rng = np.random.default_rng(71)
    cfg = sweep.SweepConfig(duration=2.0)
    play = sweep.build_playback(cfg)

    def fractional_impulse(delay, n, taps=64):
        h = np.zeros(n)
        idx = np.arange(int(np.floor(delay)) - taps, int(np.floor(delay)) + taps + 1)
        h[idx] = np.sinc(idx - delay) * np.kaiser(len(idx), 8.0)
        return h

    def rec(ir):
        y = np.convolve(play, ir)[:len(play)]
        return y + 1e-5 * rng.standard_normal(len(y))

    cases = {
        "clean": [fractional_impulse(480.37, 4096) + 0.3 * fractional_impulse(1080, 4096)] * 2,
        "louder_later": [0.7 * fractional_impulse(480, 4096) + fractional_impulse(624, 4096)],
        "blocked": [0.35 * fractional_impulse(480, 4096) + fractional_impulse(624, 4096)],
        "moved": [fractional_impulse(480, 4096), fractional_impulse(500, 4096)],
    }
    for name, irs in cases.items():
        recs = [rec(ir) for ir in irs]
        files = ",".join(str(save(tmp_path, f"{name}_{i}", r)) for i, r in enumerate(recs))
        cpp = run_cli("analyze", "--fs", cfg.fs, "--duration", cfg.duration, "--position", f"{name}={files}")["captures"][0]
        py = alignment.estimate_arrival(capture.analyze_sweep_capture(name, recs, cfg))
        assert cpp["arrival"]["ms"] == pytest.approx(py.ms, abs=1e-9), name
        assert cpp["arrival"]["confidence"] == py.confidence and cpp["arrival"]["reasons"] == py.reasons, name


def test_sub_alignment_matches_python(tmp_path):
    """The mains (high-passed at 80 Hz) and a sub measured at the same spot: the
    crossover, each one's SNR over it, and the suggested delay and polarity."""
    from roomeq import grading

    rng = np.random.default_rng(83)
    play = sweep.build_playback(CFG)
    tops = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=80.0, hp_order=4, latency_ms=10.0))
    sub = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=25.0, hp_order=4, lp_hz=80.0, lp_order=4, peq=(), latency_ms=6.0))
    noise = roomsim.NoiseSpec(pink_dbfs=-55.0)
    args, caps = [], []
    for name, room, band in (("M", tops, (250.0, 4000.0)), ("S", sub, (40.0, 100.0))):
        recs = [room.play(play, 2, noise, rng) for _ in range(2)]
        files = ",".join(str(save(tmp_path, f"{name}_{i}", r)) for i, r in enumerate(recs))
        args += ["--position", f"{name}={files}"]
        acfg = capture.AnalysisConfig(grading=grading.GradingConfig(passband=band))
        caps.append(capture.analyze_sweep_capture(name, recs, CFG, acfg))
    main, low = alignment.low_response(caps[0]), alignment.low_response(caps[1])
    lo, hi = alignment.crossover_region(main, low)
    snrs = [alignment.band_snr(c.grade.bands, lo, hi) for c in caps]

    for settings in ((0.0, False, 0.0, False), (1.5, True, 2.0, False), (0.0, False, 3.0, True)):
        md, mi, sd, si = settings
        cpp = run_cli("align-sub", "--fs", CFG.fs, "--duration", CFG.duration, *args, "--main-delay", md,
                      "--main-invert", int(mi), "--sub-delay", sd, "--sub-invert", int(si))
        py = alignment.align_sub(main, low, md, mi, sd, si, *snrs)
        assert cpp["region"] == pytest.approx([lo, hi], abs=1e-9)
        assert [cpp["main_snr_db"], cpp["sub_snr_db"]] == pytest.approx(snrs, abs=TOL_DB)
        assert cpp["ok"] == py.ok and cpp["invert"] == py.invert, settings
        assert cpp["region_hz"] == pytest.approx(list(py.region_hz), abs=1e-9)
        assert cpp["crossing_hz"] == pytest.approx(py.crossing_hz, abs=1e-9)
        assert cpp["delay_ms"] == pytest.approx(py.delay_ms, abs=1e-9), settings
        for key in ("summed_db", "improvement_db", "efficiency_db"):
            assert cpp[key] == pytest.approx(getattr(py, key), abs=TOL_DB), key
        assert cpp["confidence"] == py.confidence and cpp["reasons"] == py.reasons and cpp["note"] == py.note, settings
