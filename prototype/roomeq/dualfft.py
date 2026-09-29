"""Program-material fallback: dual-FFT transfer function with coherence.

Reference x = what the plugin sends (input or output tap), measured
y = the measurement mic. H1 = Gxy / Gxx is unbiased by mic-side noise
(crowd, load-in), and coherence tells us which bins to trust.

Clock drift: when the output and the mic run on different clocks (two
interfaces, an aggregate device), the delay between them slides during the
capture. Averaged over 20-30 s, the high frequencies' phase turns over and
the cross-spectra cancel: coherence collapses from a few hundred Hz up and
|H| reads low, although a sweep (analysed 5 s at a time, magnitude only) is
fine. So the delay is measured in short blocks along the capture, a line is
fitted through them, and a drift of 0.2 ppm or more is resampled out of the
mic signal before the estimate.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np

from .spectrum import smooth_power
from .sweep import next_pow2


@dataclass(frozen=True)
class DualFFTConfig:
    # Rounded up to a power of two at the session rate. Long enough to keep
    # low-frequency reverb inside the frame; shorter frames read low below ~100 Hz.
    fft_seconds: float = 1.0
    overlap: float = 0.5
    max_delay_s: float = 1.0
    drift_block_s: float = 3.0       # delay measured in blocks this long, half overlapping
    drift_min_ppm: float = 0.2       # less is left alone: coherence stays above 0.97 at 16 kHz over 30 s
    drift_max_ppm: float = 1000.0    # more isn't a clock difference
    drift_tolerance: float = 2.0     # samples: blocks further than this from the fitted line disagree


def estimate_delay(x: np.ndarray, y: np.ndarray, fs: float, max_delay_s: float = 1.0) -> int:
    """Delay of y relative to x in samples (GCC-PHAT, non-negative lags only)."""
    n = next_pow2(len(x) + len(y))
    R = np.conj(np.fft.rfft(x, n)) * np.fft.rfft(y, n)
    R /= np.maximum(np.abs(R), 1e-20)
    r = np.fft.irfft(R, n)
    max_lag = int(max_delay_s * fs)
    return int(np.argmax(r[:max_lag + 1]))


def _phat_spectrum(x: np.ndarray, y: np.ndarray) -> tuple[np.ndarray, int]:
    n = next_pow2(len(x) + len(y))
    R = np.conj(np.fft.rfft(x, n)) * np.fft.rfft(y, n)
    R /= np.maximum(np.abs(R), 1e-20)
    return R, n


def _refine_peak(R: np.ndarray, n: int, k: int, iterations: int = 6) -> float:
    """Sub-sample peak of the band-limited correlation r(tau) = irfft(R) near integer lag k.

    Evaluated from the spectrum, so it is exact between samples (a parabola
    through three samples is biased by up to ~0.4 samples, and the bias moves
    as a drifting delay slides between samples). Newton steps on r'(tau) = 0.
    """
    w = 2 * np.pi * np.arange(len(R)) / n
    scale = np.full(len(R), 2.0)
    scale[0] = 1.0
    scale[-1] = 1.0                                        # DC and Nyquist appear once
    a = scale * R
    tau = float(k)
    for _ in range(iterations):
        e = np.exp(1j * w * tau)
        d1 = np.real(np.sum(1j * w * a * e))
        d2 = np.real(np.sum(-(w ** 2) * a * e))
        if d2 >= 0:
            break
        step = -d1 / d2
        tau = min(max(tau + step, k - 1.0), k + 1.0)
        if abs(step) < 1e-6:
            break
    return tau


@dataclass
class DriftEstimate:
    ppm: float                     # delay growth; + means the mic's clock runs fast. 0 unless "corrected"
    status: str                    # "none", "corrected", "unsteady" (the delay jumps around) or "unknown"
    blocks: int = 0                # blocks with a clear delay
    measured_ppm: float = float("nan")


def _block_delays(x: np.ndarray, y: np.ndarray, fs: float, cfg: DualFFTConfig) -> tuple[np.ndarray, np.ndarray]:
    """Delay (sub-sample) and time of each block with a clear correlation peak."""
    block = int(round(cfg.drift_block_s * fs))
    hop = block // 2
    max_lag = int(cfg.max_delay_s * fs)
    times, delays, peaks = [], [], []
    s = 0
    while s + block <= len(x) and s + block + max_lag <= len(y):
        R, n = _phat_spectrum(x[s:s + block], y[s:s + block + max_lag])
        r = np.fft.irfft(R, n)[:max_lag + 1]
        k = int(np.argmax(r))
        times.append(s + block / 2)
        delays.append(_refine_peak(R, n, k))
        peaks.append(r[k])
        s += hop
    if not peaks:
        return np.zeros(0), np.zeros(0)
    peaks = np.asarray(peaks)
    keep = peaks >= 0.3 * peaks.max()
    return np.asarray(times)[keep], np.asarray(delays)[keep]


def _fit_line(t: np.ndarray, d: np.ndarray, cfg: DualFFTConfig) -> tuple[float, int]:
    """Theil-Sen slope (samples of delay per sample) and how many blocks sit on the line."""
    slopes = [(d[j] - d[i]) / (t[j] - t[i]) for i in range(len(t)) for j in range(i + 1, len(t))]
    slope = float(np.median(slopes))
    offset = float(np.median(d - slope * t))
    return slope, int(np.sum(np.abs(d - (offset + slope * t)) <= cfg.drift_tolerance))


def estimate_drift(x: np.ndarray, y: np.ndarray, fs: float, cfg: DualFFTConfig = DualFFTConfig()) -> DriftEstimate:
    """How fast the delay of y behind x changes along the capture.

    GCC-PHAT in half-overlapping blocks (full delay search each time, so a
    wrong first guess can't throw it off), each peak refined between samples
    on the band-limited correlation, then a Theil-Sen line (median of pairwise
    slopes), which ignores the odd block that locked onto a reflection or a
    quiet passage. A fast drift smears each block's peak (40 ppm slides 6
    samples in 3 s), so when there is one, what's left after taking the first
    estimate out is measured again, on sharp peaks, and added to it.
    """
    t, d = _block_delays(x, y, fs, cfg)
    if len(t) < 3:
        return DriftEstimate(0.0, "unknown", int(len(t)))
    slope, inliers = _fit_line(t, d, cfg)
    ppm = slope * 1e6
    if cfg.drift_min_ppm <= abs(ppm) <= cfg.drift_max_ppm:
        t2, d2 = _block_delays(x, remove_drift(y, ppm), fs, cfg)
        if len(t2) >= 3:
            slope2, inliers = _fit_line(t2, d2, cfg)
            ppm = ((1 + ppm * 1e-6) * (1 + slope2) - 1) * 1e6
            t = t2
    if (len(t) >= 4 and inliers < 0.6 * len(t)) or abs(ppm) > cfg.drift_max_ppm:
        return DriftEstimate(0.0, "unsteady", int(len(t)), ppm)
    if abs(ppm) < cfg.drift_min_ppm:
        return DriftEstimate(0.0, "none", int(len(t)), ppm)
    return DriftEstimate(ppm, "corrected", int(len(t)), ppm)


# Windowed-sinc interpolation: 64 taps, Kaiser (beta 8), 512 fractional phases.
_TAPS = 64
_PHASES = 512
_BETA = 8.0


def _bessel_i0(v: float) -> float:
    term, total, m = 1.0, 1.0, 1
    while term > 1e-17 * total:
        term *= (v / (2 * m)) ** 2
        total += term
        m += 1
    return total


def _sinc_table() -> np.ndarray:
    half = _TAPS // 2
    k = np.arange(-half + 1, half + 1)                     # -31 .. 32
    table = np.zeros((_PHASES + 1, _TAPS))
    norm = _bessel_i0(_BETA)
    for p in range(_PHASES + 1):
        u = k - p / _PHASES
        w = np.array([_bessel_i0(_BETA * math.sqrt(max(0.0, 1.0 - (ui / half) ** 2))) / norm for ui in u])
        h = np.sinc(u) * w
        table[p] = h / h.sum()
    return table


_TABLE = _sinc_table()


def interpolate_at(y: np.ndarray, positions: np.ndarray) -> np.ndarray:
    """y sampled at fractional `positions` (zeros outside y)."""
    half = _TAPS // 2
    base = np.floor(positions).astype(np.int64)
    ph = (positions - base) * _PHASES
    pi = np.minimum(np.floor(ph).astype(np.int64), _PHASES - 1)
    a = (ph - pi)[:, None]
    coefs = (1 - a) * _TABLE[pi] + a * _TABLE[pi + 1]
    idx = base[:, None] + np.arange(-half + 1, half + 1)[None, :]
    valid = (idx >= 0) & (idx < len(y))
    vals = np.where(valid, y[np.clip(idx, 0, len(y) - 1)], 0.0)
    return np.sum(vals * coefs, axis=1)


def drift_notes(drift: DriftEstimate | None) -> list[str]:
    """What a capture says about the clocks (none when they agree)."""
    if drift is None:
        return []
    if drift.status == "corrected":
        return [f"output and mic clocks differ by {abs(drift.ppm):.1f} ppm (corrected)"]
    if drift.status == "unsteady":
        return ["the delay between the output and the mic jumps around: use one audio interface for both"]
    return []


def remove_drift(y: np.ndarray, ppm: float, chunk: int = 32768) -> np.ndarray:
    """y resampled so a delay growing at `ppm` becomes constant: y'[n] = y(n (1 + ppm/1e6))."""
    out = np.empty(len(y))
    rate = 1.0 + ppm * 1e-6
    for s in range(0, len(y), chunk):
        n = np.arange(s, min(s + chunk, len(y)), dtype=np.float64)
        out[s:s + len(n)] = interpolate_at(y, n * rate)
    return out


@dataclass
class TransferEstimate:
    freqs: np.ndarray
    H: np.ndarray                  # H1 estimate (complex)
    coherence: np.ndarray          # magnitude-squared coherence, 0..1
    gxx: np.ndarray
    gyy: np.ndarray
    n_segments: int
    effective_averages: float      # program duration / FFT length
    delay: int                     # samples, removed before the estimate
    fs: float
    nfft: int
    drift: DriftEstimate | None = None


def transfer_function(x: np.ndarray, y: np.ndarray, fs: float,
                      cfg: DualFFTConfig = DualFFTConfig(), nfft: int | None = None) -> TransferEstimate:
    drift = estimate_drift(x, y, fs, cfg)
    if drift.status == "corrected":
        y = remove_drift(y, drift.ppm)
    delay = estimate_delay(x, y, fs, cfg.max_delay_s)
    y_al = y[delay:]
    x_al = x[:len(y_al)]
    n = min(len(x_al), len(y_al))
    x_al, y_al = x_al[:n], y_al[:n]

    nfft = nfft or next_pow2(int(cfg.fft_seconds * fs))
    hop = max(1, int(nfft * (1 - cfg.overlap)))
    win = np.hanning(nfft)
    starts = range(0, n - nfft + 1, hop)

    gxx = np.zeros(nfft // 2 + 1)
    gyy = np.zeros(nfft // 2 + 1)
    gxy = np.zeros(nfft // 2 + 1, dtype=complex)
    k = 0
    for s in starts:
        X = np.fft.rfft(win * x_al[s:s + nfft])
        Y = np.fft.rfft(win * y_al[s:s + nfft])
        gxx += np.abs(X) ** 2
        gyy += np.abs(Y) ** 2
        gxy += np.conj(X) * Y
        k += 1
    if k == 0:
        raise ValueError("program excerpt shorter than one FFT frame")
    gxx /= k
    gyy /= k
    gxy /= k

    with np.errstate(invalid="ignore", divide="ignore"):
        H = np.where(gxx > 0, gxy / gxx, 0)
        coh = np.where((gxx > 0) & (gyy > 0), np.abs(gxy) ** 2 / (gxx * gyy), 0.0)
    return TransferEstimate(freqs=np.fft.rfftfreq(nfft, 1 / fs), H=H, coherence=np.clip(coh, 0, 1),
                            gxx=gxx, gyy=gyy, n_segments=k, effective_averages=n / nfft,
                            delay=delay, fs=fs, nfft=nfft, drift=drift)


def noise_power_h_domain(est: TransferEstimate) -> np.ndarray:
    """Noise in |H|^2 units, comparable to a sweep capture's noise estimate.

    Incoherent output power (1 - coh) * Gyy, referred to the input via Gxx
    and reduced by the number of independent averages. Bins the program
    barely excites come out noisy, which is exactly right.
    """
    with np.errstate(invalid="ignore", divide="ignore"):
        nop = (1 - est.coherence) * est.gyy
        return np.where(est.gxx > 0, nop / (est.gxx * est.effective_averages), np.inf)


def excitation_gate(est: TransferEstimate, floor_db: float = -30.0) -> np.ndarray:
    """1 where the program excited a bin, 0 where Gxx is `floor_db` below its
    local 1/3-octave level (gaps between notes, nothing below the kick)."""
    local = smooth_power(est.freqs, est.gxx, 3, np.maximum(est.freqs, est.freqs[1]))
    with np.errstate(invalid="ignore", divide="ignore"):
        return (est.gxx > local * 10 ** (floor_db / 10)).astype(float)
