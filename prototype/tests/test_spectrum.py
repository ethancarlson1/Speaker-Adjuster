import numpy as np
import pytest

from roomeq import spectrum


FS = 48000
NFFT = 32768
FREQS = np.fft.rfftfreq(NFFT, 1 / FS)
GRID = spectrum.log_freq_grid(20, 20000, 48)


def test_log_grid_spacing():
    g = spectrum.log_freq_grid(20, 20000, 48)
    assert g[0] == 20
    assert np.allclose(np.diff(np.log2(g)), 1 / 48)
    assert g[-1] <= 20000


@pytest.mark.parametrize("fraction", [3, 6, 24])
def test_smoothing_keeps_flat_spectrum_flat(fraction):
    out = spectrum.smooth_power(FREQS, np.full_like(FREQS, 2.0), fraction, GRID)
    assert np.allclose(out, 2.0)


def test_smoothing_preserves_mean_power_of_noise():
    rng = np.random.default_rng(1)
    p = rng.exponential(1.0, len(FREQS))
    out = spectrum.smooth_power(FREQS, p, 3, spectrum.log_freq_grid(2000, 16000, 12))
    assert np.allclose(out, 1.0, atol=0.25)
    assert np.mean(out) == pytest.approx(1.0, abs=0.03)


def test_wider_smoothing_fills_a_notch_more():
    p = np.ones_like(FREQS)
    p[(FREQS > 980) & (FREQS < 1020)] = 1e-4
    at_1k = np.array([1000.0])
    depth = {n: spectrum.to_db(spectrum.smooth_power(FREQS, p, n, at_1k))[0] for n in (3, 6, 24)}
    assert depth[3] > depth[6] > depth[24]
    assert depth[3] > -1.0


def test_smoothing_bandwidth_matches_fraction():
    """A single-bin impulse spreads over exactly 1/N octave of output points."""
    p = np.zeros_like(FREQS)
    k = int(round(4000 / (FREQS[1] - FREQS[0])))
    p[k] = 1.0
    grid = spectrum.log_freq_grid(2000, 8000, 480)
    out = spectrum.smooth_power(FREQS, p, 3, grid)
    width_octaves = np.count_nonzero(out > 0) / 480
    assert width_octaves == pytest.approx(1 / 3, abs=0.01)


def test_weights_exclude_bins():
    p = np.ones_like(FREQS)
    p[FREQS > 1000] = 100.0
    w = np.where(FREQS > 1000, 0.0, 1.0)
    out = spectrum.smooth_power(FREQS, p, 3, np.array([1000.0]), w)
    assert out[0] == pytest.approx(1.0)


def test_zero_weight_nan_bins_do_not_poison_neighbours():
    p = np.ones_like(FREQS)
    p[FREQS < 20] = np.nan
    w = np.where(FREQS < 20, 0.0, 1.0)
    out = spectrum.smooth_power(FREQS, p, 3, GRID, w)
    assert np.allclose(out, 1.0)


def test_rebin_preserves_level():
    fine = np.fft.rfftfreq(2 * NFFT, 1 / FS)
    p = 1 + 0.5 * np.sin(fine / 300)
    out = spectrum.rebin_power(fine, p, FREQS)
    ref = 1 + 0.5 * np.sin(FREQS / 300)
    assert np.allclose(out[1:-1], ref[1:-1], atol=1e-3)


def test_ir_window_shape():
    w, n_pre = spectrum.ir_window(spectrum.WindowConfig(pre=0.02, post=0.5), FS)
    assert len(w) == int(0.52 * FS)
    assert n_pre == int(0.02 * FS)
    assert w[0] == 0.0
    assert np.all(w[n_pre // 2:n_pre + int(0.3 * FS)] == 1.0)
    assert w[-1] < 1e-3


@pytest.mark.parametrize("f,text", [(88.4, "88 Hz"), (176.8, "180 Hz"), (1414, "1.4 kHz"), (11314, "11 kHz"), (22.1, "22 Hz")])
def test_format_hz(f, text):
    assert spectrum.format_hz(f) == text
