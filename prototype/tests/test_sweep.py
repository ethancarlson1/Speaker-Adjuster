import numpy as np
import pytest
import scipy.signal as ss

from roomeq import spectrum, sweep
from roomeq.roomsim import _peaking_sos


def response_db(h, fs, freqs):
    _, H = ss.freqz(h, worN=freqs, fs=fs)
    return 20 * np.log10(np.abs(H))


def test_sweep_follows_exponential_frequency_law():
    cfg = sweep.SweepConfig(duration=2.0)
    x = sweep.generate_sweep(cfg)
    phase = np.unwrap(np.angle(ss.hilbert(x)))
    inst_f = np.diff(phase) * cfg.fs / (2 * np.pi)
    for frac in (0.25, 0.5, 0.75):
        i = int(frac * len(x))
        expected = cfg.f1 * np.exp(i / cfg.fs / cfg.rate)
        assert inst_f[i - 50:i + 50].mean() == pytest.approx(expected, rel=0.01)


def test_sweep_level_and_fades():
    cfg = sweep.SweepConfig(duration=2.0, level_dbfs=-12.0)
    x = sweep.generate_sweep(cfg)
    assert np.max(np.abs(x)) == pytest.approx(10 ** (-12 / 20), rel=1e-3)
    assert abs(x[0]) < 1e-6 and abs(x[-1]) < 1e-3


@pytest.mark.parametrize("fs,duration", [(44100, 2.0), (48000, 5.0), (96000, 2.0), (48000, 10.0)])
def test_identity_loop_gives_flat_unit_impulse(fs, duration):
    cfg = sweep.SweepConfig(fs=fs, duration=duration)
    x = sweep.generate_sweep(cfg)
    dec = sweep.deconvolve(sweep.build_playback(cfg, x), x, fs, cfg.f1, cfg.f2)
    arrival = sweep.find_arrival(dec, cfg.n_preroll)
    assert arrival == dec.zero + cfg.n_preroll
    seg = dec.h[arrival - fs:arrival + fs]
    # The top half-kHz sits under the sweep's fade-out; see test below for the edges.
    freqs = np.geomspace(cfg.f1, 19500, 200)
    assert np.max(np.abs(response_db(seg, fs, freqs))) < 0.01


@pytest.mark.parametrize("fs", [44100, 48000, 96000])
def test_analysis_window_accuracy_at_band_edges(fs):
    """With the real analysis window (-50 ms / +500 ms), the zero-phase
    band-limit's pre-ringing costs at most ~0.35 dB, and only below 25 Hz."""
    cfg = sweep.SweepConfig(fs=fs, duration=2.0)
    x = sweep.generate_sweep(cfg)
    dec = sweep.deconvolve(sweep.build_playback(cfg, x), x, fs, cfg.f1, cfg.f2)
    w, n_pre = spectrum.ir_window(spectrum.WindowConfig(), fs)
    start = dec.zero + cfg.n_preroll - n_pre
    nfft = spectrum.response_nfft(spectrum.WindowConfig(), fs)
    f = np.fft.rfftfreq(nfft, 1 / fs)
    p = np.abs(np.fft.rfft(dec.h[start:start + len(w)] * w, nfft)) ** 2
    grid = spectrum.log_freq_grid(20, 20000, 24)
    db = spectrum.to_db(spectrum.smooth_power(f, p, 6, grid, ((f >= 20) & (f <= 20000)).astype(float)))
    assert np.max(np.abs(db[grid < 25])) < 0.5
    assert np.max(np.abs(db[(grid >= 25) & (grid <= 19000)])) < 0.35


def test_recovers_known_filter_and_delay():
    cfg = sweep.SweepConfig(duration=2.0)
    fs = cfg.fs
    sos = np.vstack([ss.butter(2, 80, "highpass", fs=fs, output="sos"), _peaking_sos(1000, -6, 1.0, fs)])
    delay = 1234
    x = sweep.generate_sweep(cfg)
    play = sweep.build_playback(cfg, x)
    rec = np.concatenate([np.zeros(delay), ss.sosfilt(sos, play)])[:len(play)]
    dec = sweep.deconvolve(rec, x, fs, cfg.f1, cfg.f2)

    arrival = sweep.find_arrival(dec, cfg.n_preroll)
    imp = np.zeros(4096)
    imp[0] = 1
    true_ir = ss.sosfilt(sos, imp)
    assert arrival - dec.zero - cfg.n_preroll == delay + int(np.argmax(np.abs(true_ir)))

    start = dec.zero + cfg.n_preroll + delay
    measured = dec.h[start - fs // 5:start + fs // 2]
    freqs = np.geomspace(40, 18000, 200)
    _, H = ss.sosfreqz(sos, worN=freqs, fs=fs)
    assert np.max(np.abs(response_db(measured, fs, freqs) - 20 * np.log10(np.abs(H)))) < 0.1


def test_harmonic_distortion_lands_before_linear_ir():
    cfg = sweep.SweepConfig(duration=2.0)
    x = sweep.generate_sweep(cfg)
    play = sweep.build_playback(cfg, x)
    amp = 10 ** (cfg.level_dbfs / 20)
    a2 = 2 * 0.03 / amp                       # 2nd harmonic at -30 dB
    up = ss.resample_poly(play, 4, 1)         # oversample so 2f above Nyquist can't alias
    distorted = ss.resample_poly(up + a2 * up ** 2, 1, 4)[:len(play)]
    clean = sweep.deconvolve(play, x, cfg.fs, cfg.f1, cfg.f2)
    dirty = sweep.deconvolve(distorted, x, cfg.fs, cfg.f1, cfg.f2)
    t0 = dirty.zero + cfg.n_preroll

    # The 2nd-harmonic IR shows up at -L*ln(2) before the linear IR.
    lag2 = int(round(cfg.rate * np.log(2) * cfg.fs))
    near = np.abs(dirty.h[t0 - lag2 - 200:t0 - lag2 + 200]).max()
    assert near > 0.01

    # ... and the linear part is untouched.
    freqs = np.geomspace(40, 18000, 200)
    seg = slice(t0 - cfg.fs // 20, t0 + cfg.fs // 5)
    err = response_db(dirty.h[seg], cfg.fs, freqs) - response_db(clean.h[seg], cfg.fs, freqs)
    assert np.max(np.abs(err)) < 0.05


def test_fractional_peak_offset():
    rng = np.random.default_rng(0)
    a = rng.standard_normal(4096)
    a = ss.sosfilt(ss.butter(4, 0.3, output="sos"), a)
    f = np.fft.rfftfreq(len(a))
    for true_shift in (0.3, -0.45, 2.25):
        b = np.fft.irfft(np.fft.rfft(a) * np.exp(-2j * np.pi * f * true_shift), len(a))
        assert sweep.fractional_peak_offset(a, b, max_lag=8) == pytest.approx(true_shift, abs=0.05)
