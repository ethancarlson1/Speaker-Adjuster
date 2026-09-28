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

from roomeq import averaging, capture, roomsim, sweep
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

    # The scenario must exercise every grade path, or the comparison proves little.
    assert {c.grade.overall.label for c in py_caps} >= {"pass", "redo"}

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
