"""End-to-end checks against the simulated room (ground truth is known)."""

import numpy as np
import pytest

from roomeq import averaging, capture, roomsim, spectrum, sweep
from roomeq.grading import Grade

CFG = sweep.SweepConfig(duration=5.0)
SWEEP = sweep.generate_sweep(CFG)
PLAY = sweep.build_playback(CFG, SWEEP)
GRID = spectrum.log_freq_grid(40, 16000, 24)


def truth_power(sim, pos):
    """Linear system response through the same analysis window as a capture."""
    _, power, pk = capture.windowed_response(sim.linear_ir(pos), CFG.fs)
    return power, pk


def smoothed_db(freqs, power, fraction=6, weights=None):
    return spectrum.to_db(spectrum.smooth_power(freqs, power, fraction, GRID, weights))


def measure(sim, positions, noise, seed, repeats=2):
    rng = np.random.default_rng(seed)
    return [capture.analyze_sweep_capture(f"P{p + 1}", [sim.play(PLAY, p, noise, rng) for _ in range(repeats)], CFG, sweep=SWEEP)
            for p in positions]


def test_single_capture_matches_truth_and_delay(sim):
    caps = measure(sim, range(len(sim.positions)), roomsim.NoiseSpec(), seed=10)
    for pos, cap in enumerate(caps):
        truth, pk = truth_power(sim, pos)
        err = smoothed_db(cap.freqs, cap.power) - smoothed_db(cap.freqs, truth)
        assert np.max(np.abs(err)) < 0.6, f"P{pos + 1}"
        assert cap.delays_ms[0] == pytest.approx(1000 * pk / CFG.fs, abs=1000 / CFG.fs)
        assert cap.grade.overall == Grade.PASS


def test_three_position_average_matches_truth_in_noisy_room(sim):
    noise = roomsim.NoiseSpec(pink_dbfs=-50.0, bursts=2, burst_dbfs=-25.0)
    caps = measure(sim, (0, 1, 2), noise, seed=11)
    avg = averaging.power_average(caps[0].freqs, [c.power for c in caps],
                                  [averaging.band_mask_weights(c.freqs, c.grade) for c in caps])
    truths = [truth_power(sim, p)[0] for p in (0, 1, 2)]
    ref = averaging.power_average(caps[0].freqs, truths, [np.ones_like(truths[0])] * 3)
    err = smoothed_db(avg.freqs, avg.power) - smoothed_db(ref.freqs, ref.power)
    assert np.max(np.abs(err)) < 1.0


def test_average_is_repeatable_across_sessions(sim):
    """Same positions, different noise (incl. random bangs): averages agree."""
    noise = roomsim.NoiseSpec(pink_dbfs=-50.0, bursts=2, burst_dbfs=-25.0)
    curves = []
    for seed in (21, 22):
        caps = measure(sim, (0, 1, 2), noise, seed)
        avg = averaging.power_average(caps[0].freqs, [c.power for c in caps],
                                      [averaging.band_mask_weights(c.freqs, c.grade) for c in caps])
        curves.append(smoothed_db(avg.freqs, avg.power))
    assert np.max(np.abs(curves[0] - curves[1])) < 0.5


def test_truck_rumble_is_graded_redo_with_reason(sim):
    cap = measure(sim, (2,), roomsim.NoiseSpec(rumble_dbfs=-25.0), seed=12)[0]
    assert cap.grade.overall == Grade.REDO
    assert any(r.startswith("redo: low-end noise too high below") for r in cap.grade.reasons)
    # ... and it still contributes to the average above the rumble.
    w = averaging.band_mask_weights(cap.freqs, cap.grade)
    assert w[np.argmin(np.abs(cap.freqs - 1000))] == 1.0


def test_loud_bang_during_one_repeat_is_flagged(sim):
    rng = np.random.default_rng(13)
    recs = [sim.play(PLAY, 1, roomsim.NoiseSpec(), rng),
            sim.play(PLAY, 1, roomsim.NoiseSpec(bursts=1, burst_dbfs=0.0, burst_times=(2.0,)), rng)]
    cap = capture.analyze_sweep_capture("P2", recs, CFG, sweep=SWEEP)
    assert cap.grade.overall >= Grade.MARGINAL
    assert any("repeat sweeps differ" in r for r in cap.grade.reasons)


def test_program_material_matches_sweep(sim):
    rng = np.random.default_rng(14)
    x = roomsim.synthetic_program(30.0, CFG.fs, rng)
    prog = capture.analyze_program_capture("walk-in", x, sim.play(x, 0, roomsim.NoiseSpec(), rng), CFG.fs)
    ref = measure(sim, (0,), roomsim.NoiseSpec(), seed=15, repeats=1)[0]
    err = smoothed_db(prog.freqs, prog.power, 3, prog.weight) - smoothed_db(ref.freqs, ref.power, 3)
    band = GRID >= 80
    assert np.max(np.abs(err[band])) < 1.5
    assert prog.delays_ms[0] == pytest.approx(ref.delays_ms[0], abs=0.05)
