#pragma once

// Program-material fallback: dual-FFT transfer function with coherence.
// Port of prototype/roomeq/dualfft.py.
//
// Clock drift: when the output and the mic run on different clocks (two
// interfaces, an aggregate device), the delay slides during the capture and
// the averaged cross-spectra cancel at high frequencies. The delay is measured
// in short blocks along the capture, a line is fitted through them, and a
// drift of 0.2 ppm or more is resampled out of the mic signal first.

#include "roomeq/fft.h"

#include <cstddef>
#include <string>
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
    double driftBlockSeconds = 3.0;   // delay measured in blocks this long, half overlapping
    double driftMinPpm = 0.2;         // less is left alone: coherence stays above 0.97 at 16 kHz over 30 s
    double driftMaxPpm = 1000.0;      // more isn't a clock difference
    double driftTolerance = 2.0;      // samples: blocks further than this from the fitted line disagree
};

// Delay of y relative to x in samples (GCC-PHAT, non-negative lags only).
std::size_t estimateDelay (const std::vector<double>& x, const std::vector<double>& y, double fs,
                           double maxDelaySeconds = 1.0);

struct DriftEstimate
{
    enum class Status
    {
        none,        // the clocks agree (within driftMinPpm)
        corrected,   // resampled out
        unsteady,    // the delay jumps around rather than sliding steadily
        unknown      // too few blocks with a clear delay
    };

    double ppm = 0.0;                 // delay growth, + = the mic's clock runs fast. 0 unless corrected
    Status status = Status::unknown;
    int blocks = 0;                   // blocks with a clear delay
    double measuredPpm = 0.0;         // the fitted value, whatever the status (NaN if none)
};

// How fast the delay of y behind x changes along the capture: GCC-PHAT in
// half-overlapping blocks, each peak refined on the band-limited correlation,
// Theil-Sen line through them.
DriftEstimate estimateDrift (const std::vector<double>& x, const std::vector<double>& y, double fs,
                             const DualFFTConfig& cfg = {});

// y at fractional positions (64-tap Kaiser-windowed sinc, zeros outside y).
double interpolateAt (const std::vector<double>& y, double position);

// y resampled so a delay growing at ppm becomes constant: y'[n] = y(n (1 + ppm/1e6)).
std::vector<double> removeDrift (const std::vector<double>& y, double ppm);

// What a capture says about the clocks (nothing when they agree).
std::vector<std::string> driftNotes (const DriftEstimate& drift);

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
    DriftEstimate drift;
};

TransferEstimate transferFunction (const std::vector<double>& x, const std::vector<double>& y, double fs,
                                   const DualFFTConfig& cfg = {}, std::size_t nfft = 0);

// Noise in |H|^2 units, comparable to a sweep capture's noise estimate.
std::vector<double> noisePowerHDomain (const TransferEstimate& est);

// 1 where the program excited a bin, 0 where Gxx is floorDb below its local 1/3-octave level.
std::vector<double> excitationGate (const TransferEstimate& est, double floorDb = -30.0);
} // namespace roomeq
