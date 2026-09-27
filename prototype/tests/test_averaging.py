import numpy as np
import pytest
import scipy.signal as ss

from roomeq import averaging, spectrum
from roomeq.grading import grade_capture

FS = 48000
FREQS = np.fft.rfftfreq(32768, 1 / FS)
ONES = np.ones_like(FREQS)


def test_average_of_identical_spectra_is_unchanged():
    p = 1 + 0.5 * np.cos(FREQS / 500)
    res = averaging.power_average(FREQS, [p, p, p], [ONES] * 3)
    assert np.allclose(res.power, p)
    assert np.allclose(res.offsets_db, 0, atol=1e-9)


def test_power_not_db_average():
    res = averaging.power_average(FREQS, [ONES, 4 * ONES], [ONES, ONES], align_levels=False)
    assert np.allclose(res.power, 2.5)


def test_level_alignment_removes_distance_offset():
    shape = 1 + 0.5 * np.cos(FREQS / 500)
    res = averaging.power_average(FREQS, [shape, shape * 10 ** (6 / 10)], [ONES, ONES])
    assert res.offsets_db[0] == pytest.approx(3.0, abs=0.01)
    assert res.offsets_db[1] == pytest.approx(-3.0, abs=0.01)
    assert np.allclose(res.power, shape * 10 ** (3 / 10), rtol=1e-3)


def test_redo_bands_are_masked_out_of_the_average():
    noise = np.full_like(FREQS, 1e-4)
    noise[FREQS < 88] = 0.5                                  # this capture is bad below 88 Hz
    bad = grade_capture(FREQS, ONES, noise, f_max=20000)
    w_bad = averaging.band_mask_weights(FREQS, bad)
    assert w_bad[np.argmin(np.abs(FREQS - 40))] == 0
    assert w_bad[np.argmin(np.abs(FREQS - 1000))] == 1

    res = averaging.power_average(FREQS, [ONES, 100 * ONES], [ONES, w_bad], align_levels=False)
    assert res.power[np.argmin(np.abs(FREQS - 40))] == pytest.approx(1.0)
    assert res.power[np.argmin(np.abs(FREQS - 2000))] == pytest.approx(50.5)


def test_usable_range_finds_minus_10_db_points():
    grid = spectrum.log_freq_grid(20, 20000, 48)
    _, H = ss.sosfreqz(np.vstack([ss.butter(4, 60, "highpass", fs=FS, output="sos"),
                                  ss.butter(2, 16000, "lowpass", fs=FS, output="sos")]), worN=grid, fs=FS)
    lo, hi = averaging.usable_range(grid, 20 * np.log10(np.abs(H)))
    # 4th-order Butterworth is -10 dB at ~0.76*fc; 2nd-order at ~1.4*fc (capped by the grid).
    assert lo == pytest.approx(60 * 0.76, rel=0.03)
    assert hi > 19000


def test_quick_mode_strength_grows_with_good_positions():
    policies = [averaging.quick_mode_policy(n, user_fraction=6) for n in (1, 2, 3, 5)]
    assert [p.smoothing_fraction for p in policies] == [2, 3, 6, 6]
    assert [p.strength for p in policies] == sorted(p.strength for p in policies)
    assert policies[0].max_correction_db == 3.0
    assert policies[-1].max_correction_db is None
