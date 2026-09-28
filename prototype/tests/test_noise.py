import numpy as np
import pytest

from roomeq import capture, noise, roomsim, spectrum, sweep


def band_level_db(x, fs, lo, hi):
    f = np.fft.rfftfreq(len(x), 1 / fs)
    p = np.abs(np.fft.rfft(x)) ** 2
    sel = (f >= lo) & (f < hi)
    return 10 * np.log10(np.mean(p[sel]))


def test_pink_slope_peak_and_crest():
    cfg = noise.NoiseConfig(duration=10.0, level_dbfs=-12.0)
    x = noise.generate_pink_noise(cfg)
    assert len(x) == cfg.n_noise
    assert np.max(np.abs(x)) == pytest.approx(10 ** (-12 / 20), rel=1e-9)
    crest_db = 20 * np.log10(np.max(np.abs(x)) / np.sqrt(np.mean(x ** 2)))
    assert 9.0 < crest_db < 10.5                         # peaks clipped at 3 sigma
    # -3 dB/octave per-bin density: 4 octaves apart -> ~12 dB lower.
    drop = band_level_db(x, cfg.fs, 90, 110) - band_level_db(x, cfg.fs, 1440, 1760)
    assert drop == pytest.approx(12.0, abs=1.5)
    assert abs(x[0]) < 1e-6 and abs(x[-1]) < 1e-3      # faded


def test_playback_adds_tail():
    cfg = noise.NoiseConfig(duration=2.0, tail=1.0)
    play = noise.build_noise_playback(cfg)
    assert len(play) == cfg.n_noise + cfg.n_tail
    assert np.all(play[cfg.n_noise:] == 0)


def test_pink_noise_capture_matches_sweep(sim):
    rng = np.random.default_rng(41)
    ncfg = noise.NoiseConfig(duration=20.0)
    x = noise.generate_pink_noise(ncfg)
    cap = capture.analyze_program_capture("noise", x, sim.play(noise.build_noise_playback(ncfg, x), 0,
                                                                roomsim.NoiseSpec(), rng), ncfg.fs)
    scfg = sweep.SweepConfig(duration=5.0)
    ref = capture.analyze_sweep_capture("sweep", [sim.play(sweep.build_playback(scfg), 0, roomsim.NoiseSpec(), rng)
                                                  for _ in range(2)], scfg)
    grid = spectrum.log_freq_grid(60, 16000, 24)
    a = spectrum.to_db(spectrum.smooth_power(cap.freqs, cap.power, 6, grid, cap.weight))
    b = spectrum.to_db(spectrum.smooth_power(ref.freqs, ref.power, 6, grid, ref.weight))
    assert np.max(np.abs(a - b)) < 1.2
    assert cap.grade.overall.label == "pass"
