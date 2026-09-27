import numpy as np

from roomeq.grading import Grade, GradingConfig, grade_capture

FS = 48000
FREQS = np.fft.rfftfreq(32768, 1 / FS)
FLAT = np.ones_like(FREQS)


def noise_with(level, where):
    n = np.full_like(FREQS, 1e-4)      # 40 dB SNR baseline
    n[where] = level
    return n


def test_clean_capture_passes():
    g = grade_capture(FREQS, FLAT, noise_with(1e-4, FREQS < 0), f_max=20000)
    assert g.overall == Grade.PASS
    assert g.reasons == ["all bands ≥ 20 dB SNR"]


def test_low_end_noise_asks_for_redo_with_range():
    g = grade_capture(FREQS, FLAT, noise_with(0.3, FREQS < 88), f_max=20000)
    assert g.overall == Grade.REDO
    assert g.reasons == ["redo: low-end noise too high below 88 Hz (SNR 5 dB)"]


def test_high_frequency_and_mid_band_wording():
    hf = grade_capture(FREQS, FLAT, noise_with(0.05, FREQS > 5657), f_max=20000)
    assert hf.overall == Grade.MARGINAL
    assert hf.reasons == ["marginal: high-frequency noise too high above 5.7 kHz (SNR 13 dB)"]

    mid = grade_capture(FREQS, FLAT, noise_with(0.05, (FREQS > 354) & (FREQS < 707)), f_max=20000)
    assert mid.reasons == ["marginal: noise too high 350 Hz–710 Hz (SNR 13 dB)"]


def test_bands_below_pa_range_are_not_graded():
    signal = FLAT.copy()
    signal[FREQS < 44] = 0.01          # PA has rolled off 20 dB
    noise = noise_with(0.01, FREQS < 44)
    g = grade_capture(FREQS, signal, noise, f_max=20000)
    assert g.bands[0].out_of_range and g.bands[0].grade is None
    assert g.overall == Grade.PASS
    assert g.notes == ["below the PA's range under 44 Hz (not graded)"]


def test_noise_just_above_the_range_is_reported_from_first_graded_band():
    signal = FLAT.copy()
    signal[FREQS < 44] = 0.01
    g = grade_capture(FREQS, signal, noise_with(0.3, FREQS < 88), f_max=20000)
    assert g.reasons == ["redo: low-end noise too high below 88 Hz (SNR 5 dB)"]


def test_repeat_mismatch_in_one_third_octave_is_caught():
    bumped = FLAT.copy()
    bumped[(FREQS >= 891) & (FREQS < 1122)] *= 10 ** (2.0 / 10)    # one third-octave
    g = grade_capture(FREQS, FLAT, noise_with(1e-4, FREQS < 0), [FLAT, bumped], f_max=20000)
    band_1k = g.bands[5]
    assert band_1k.spread_db is not None and 1.9 < band_1k.spread_db < 2.1
    assert g.overall == Grade.MARGINAL
    assert len(g.reasons) == 1 and g.reasons[0].startswith("marginal: repeat sweeps differ by 2.0 dB 710 Hz–1.4 kHz")


def test_noise_explained_inconsistency_is_not_double_reported():
    wobble = FLAT.copy()
    wobble[FREQS < 88] *= 10 ** (4.0 / 10)
    g = grade_capture(FREQS, FLAT, noise_with(0.3, FREQS < 88), [FLAT, wobble], f_max=20000)
    assert g.overall == Grade.REDO
    assert g.reasons == ["redo: low-end noise too high below 88 Hz (SNR 5 dB)"]


def test_thresholds_are_configurable():
    strict = GradingConfig(snr_pass_db=45.0, snr_marginal_db=30.0)
    g = grade_capture(FREQS, FLAT, noise_with(1e-4, FREQS < 0), cfg=strict, f_max=20000)
    assert g.overall == Grade.MARGINAL
