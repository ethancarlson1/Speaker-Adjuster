import numpy as np
import pytest
import scipy.signal as ss

from roomeq import dualfft

FS = 48000


def system(x, delay=300):
    sos = np.vstack([ss.butter(2, 100, "highpass", fs=FS, output="sos"),
                     ss.butter(2, 8000, "lowpass", fs=FS, output="sos")])
    y = ss.sosfilt(sos, x)
    return np.concatenate([np.zeros(delay), y])[:len(x)], sos


def test_delay_and_magnitude_with_clean_input():
    rng = np.random.default_rng(0)
    x = rng.standard_normal(10 * FS)
    y, sos = system(x, delay=300)
    est = dualfft.transfer_function(x, y, FS, nfft=8192)
    imp = np.zeros(256)
    imp[0] = 1
    assert est.delay == 300 + int(np.argmax(np.abs(ss.sosfilt(sos, imp))))
    band = (est.freqs > 200) & (est.freqs < 6000)
    _, H = ss.sosfreqz(sos, worN=est.freqs[band], fs=FS)
    err = 20 * np.log10(np.abs(est.H[band])) - 20 * np.log10(np.abs(H))
    assert np.max(np.abs(err)) < 0.2
    assert np.min(est.coherence[band]) > 0.98


def test_mic_noise_lowers_coherence_but_not_h1():
    rng = np.random.default_rng(1)
    x = rng.standard_normal(20 * FS)
    y, sos = system(x)
    noisy = y + rng.standard_normal(len(y)) * np.std(y)       # 0 dB SNR at the mic
    est = dualfft.transfer_function(x, noisy, FS, nfft=8192)
    band = (est.freqs > 500) & (est.freqs < 4000)
    assert np.median(est.coherence[band]) < 0.8
    _, H = ss.sosfreqz(sos, worN=est.freqs[band], fs=FS)
    err = 10 * np.log10(np.mean(np.abs(est.H[band]) ** 2) / np.mean(np.abs(H) ** 2))
    assert abs(err) < 0.3                                     # unbiased on average

    # Effective SNR after averaging is roughly raw SNR + 10 log10(averages).
    noise = dualfft.noise_power_h_domain(est)
    snr_db = 10 * np.log10(np.sum(np.abs(est.H[band]) ** 2) / np.sum(noise[band]))
    raw_db = 10 * np.log10(np.mean(est.coherence[band]) / (1 - np.mean(est.coherence[band])))
    assert snr_db == pytest.approx(raw_db + 10 * np.log10(est.effective_averages), abs=2.0)


def test_excitation_gate_blanks_gaps_between_harmonics():
    rng = np.random.default_rng(2)
    t = np.arange(10 * FS) / FS
    x = sum(np.sin(2 * np.pi * 200 * k * t + rng.uniform(0, 6)) for k in range(1, 21))
    x = x + 1e-4 * rng.standard_normal(len(t))
    y, _ = system(x)
    est = dualfft.transfer_function(x, y, FS, nfft=8192)
    gate = dualfft.excitation_gate(est)
    df = est.freqs[1]
    on = [int(round(200 * k / df)) for k in range(5, 20)]
    off = [int(round((200 * k + 100) / df)) for k in range(5, 20)]
    assert gate[on].mean() == 1.0
    assert gate[off].mean() == 0.0
