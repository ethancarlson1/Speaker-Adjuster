#pragma once

// Exponential (log) sine sweep: generation, deconvolution, arrival detection.
// Port of prototype/roomeq/sweep.py; see there for the timeline diagram.

#include "roomeq/fft.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace roomeq
{
struct SweepConfig
{
    double fs = 48000.0;
    double duration = 5.0;          // seconds (2 / 5 / 10)
    double f1 = 20.0;
    double f2 = 20000.0;
    double levelDbfs = -12.0;       // sweep peak level
    double fadeInOctaves = 1.0 / 6.0;
    double fadeOutOctaves = 1.0 / 24.0;
    double preroll = 0.25;          // seconds recorded before the sweep starts
    double tail = 2.0;              // seconds recorded after the sweep ends

    std::size_t nSweep() const    { return static_cast<std::size_t> (std::llround (duration * fs)); }
    std::size_t nPreroll() const  { return static_cast<std::size_t> (std::llround (preroll * fs)); }
    std::size_t nTail() const     { return static_cast<std::size_t> (std::llround (tail * fs)); }
    std::size_t nRecording() const { return nPreroll() + nSweep() + nTail(); }

    // Sweep time constant L: f(t) = f1 * exp(t / L).
    double rate() const           { return duration / std::log (f2 / f1); }
};

std::vector<double> generateSweep (const SweepConfig& cfg);

// What the plugin plays out: silence, sweep, silence (same length as the recording).
std::vector<double> buildPlayback (const SweepConfig& cfg, const std::vector<double>& sweep);

// Regularised inverse conj(X) / (|X|^2 + eps(f)): exact in [f1, f2], band-limited outside.
std::vector<cplx> inverseSpectrum (const std::vector<double>& sweep, std::size_t nfft, double fs,
                                   double f1, double f2, double regInDb = -50.0, double transitionOctaves = 1.0);

struct Deconvolved
{
    std::vector<double> h;     // impulse response incl. negative times
    std::size_t zero = 0;      // index in h of recording sample 0
    double fs = 0.0;
};

Deconvolved deconvolve (const std::vector<double>& recording, const std::vector<double>& sweep,
                        double fs, double f1, double f2);

// Index in dec.h of the main arrival (peak of |h|) at or after the sweep start.
std::size_t findArrival (const Deconvolved& dec, std::size_t preroll, double maxDelaySeconds = 1.0);

// Delay of b relative to a in samples, with parabolic sub-sample refinement.
double fractionalPeakOffset (const std::vector<double>& a, const std::vector<double>& b, int maxLag = 64);
} // namespace roomeq
