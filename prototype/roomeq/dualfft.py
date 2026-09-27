"""Program-material fallback: dual-FFT transfer function with coherence.

Reference x = what the plugin sends (input or output tap), measured
y = the measurement mic. H1 = Gxy / Gxx is unbiased by mic-side noise
(crowd, load-in), and coherence tells us which bins to trust.
"""

from __future__ import annotations

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


def estimate_delay(x: np.ndarray, y: np.ndarray, fs: float, max_delay_s: float = 1.0) -> int:
    """Delay of y relative to x in samples (GCC-PHAT, non-negative lags only)."""
    n = next_pow2(len(x) + len(y))
    R = np.conj(np.fft.rfft(x, n)) * np.fft.rfft(y, n)
    R /= np.maximum(np.abs(R), 1e-20)
    r = np.fft.irfft(R, n)
    max_lag = int(max_delay_s * fs)
    return int(np.argmax(r[:max_lag + 1]))


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


def transfer_function(x: np.ndarray, y: np.ndarray, fs: float,
                      cfg: DualFFTConfig = DualFFTConfig(), nfft: int | None = None) -> TransferEstimate:
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
                            delay=delay, fs=fs, nfft=nfft)


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
