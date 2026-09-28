#pragma once

// Program-material fallback: dual-FFT transfer function with coherence.
// Port of prototype/roomeq/dualfft.py.

#include "roomeq/fft.h"

#include <cstddef>
#include <vector>

namespace roomeq
{
struct DualFFTConfig
{
    // Rounded up to a power of two at the session rate. Long enough to keep
    // low-frequency reverb inside the frame; shorter frames read low below ~100 Hz.
    double fftSeconds = 1.0;
    double overlap = 0.5;
    double maxDelaySeconds = 1.0;
};

// Delay of y relative to x in samples (GCC-PHAT, non-negative lags only).
std::size_t estimateDelay (const std::vector<double>& x, const std::vector<double>& y, double fs,
                           double maxDelaySeconds = 1.0);

struct TransferEstimate
{
    std::vector<double> freqs;
    std::vector<cplx> H;                 // H1 estimate
    std::vector<double> coherence;       // magnitude-squared, 0..1
    std::vector<double> gxx, gyy;
    int segments = 0;
    double effectiveAverages = 0.0;      // program duration / FFT length
    std::size_t delay = 0;               // samples, removed before the estimate
    double fs = 0.0;
    std::size_t nfft = 0;
};

TransferEstimate transferFunction (const std::vector<double>& x, const std::vector<double>& y, double fs,
                                   const DualFFTConfig& cfg = {}, std::size_t nfft = 0);

// Noise in |H|^2 units, comparable to a sweep capture's noise estimate.
std::vector<double> noisePowerHDomain (const TransferEstimate& est);

// 1 where the program excited a bin, 0 where Gxx is floorDb below its local 1/3-octave level.
std::vector<double> excitationGate (const TransferEstimate& est, double floorDb = -30.0);
} // namespace roomeq
