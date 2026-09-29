"""Pink noise and music captures across two clocks (the aggregate-device case).

Uncorrected, 10 ppm is enough to fail pink noise above ~700 Hz while a sweep
at the same spot passes; with the drift measured and resampled out, the
captures pass and match the no-drift capture.
"""
import numpy as np
import pytest

from roomeq import capture, noise, roomsim

FS = 48000


def pink_capture(sim, ppm, rng):
    cfg = noise.NoiseConfig(fs=FS, duration=20.0)
    x = noise.build_noise_playback(cfg, noise.generate_pink_noise(cfg))
    return capture.analyze_program_capture("N", x, sim.play(x, 1, rng=rng, drift_ppm=ppm), FS)


def level_db(cap, lo, hi):
    band = (cap.freqs >= lo) & (cap.freqs <= hi)
    return 10 * np.log10(np.mean(cap.power[band]))


@pytest.mark.parametrize("ppm", [-25.0, 10.0, 40.0])
def test_pink_noise_passes_across_two_clocks(sim, ppm):
    rng = np.random.default_rng(50)
    ref = pink_capture(sim, 0.0, rng)
    cap = pink_capture(sim, ppm, rng)
    assert ref.grade.overall.label == "pass" and ref.drift_ppm == 0.0
    assert cap.grade.overall.label == "pass", cap.grade.reasons
    assert cap.drift_ppm == pytest.approx(ppm, abs=0.1)
    assert cap.grade.notes[-1] == f"output and mic clocks differ by {abs(cap.drift_ppm):.1f} ppm (corrected)"
    assert cap.delays_ms[0] == pytest.approx(ref.delays_ms[0], abs=0.05)
    for lo, hi in ((100, 400), (1000, 4000), (8000, 16000)):
        assert level_db(cap, lo, hi) == pytest.approx(level_db(ref, lo, hi), abs=0.2), (lo, hi)


def test_music_passes_across_two_clocks(sim):
    rng = np.random.default_rng(51)
    x = roomsim.synthetic_program(30.0, FS, rng)
    cap = capture.analyze_program_capture("M", x, sim.play(x, 3, rng=rng, drift_ppm=18.0), FS)
    assert cap.drift_ppm == pytest.approx(18.0, abs=0.2)
    assert cap.grade.overall.label != "redo", cap.grade.reasons
