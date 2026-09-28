#pragma once

// IR windowing, frequency grids, octave bands and fractional-octave smoothing.
// Port of prototype/roomeq/spectrum.py.

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace roomeq
{
// Analysis window around the main arrival (REW-style fixed window).
struct WindowConfig
{
    double pre = 0.050;        // seconds kept before the arrival peak (holds band-limit pre-ringing)
    double post = 0.500;       // seconds kept after the arrival peak
    double taperPre = 0.5;     // fraction of `pre` used for the rising half-Hann
    double taperPost = 0.25;   // fraction of `post` used for the falling half-Hann
};

struct Window
{
    std::vector<double> w;
    std::size_t nPre = 0;      // index of the arrival inside the window
};

Window irWindow (const WindowConfig& cfg, double fs);
std::size_t responseNfft (const WindowConfig& cfg, double fs);

std::vector<double> logFreqGrid (double fLo = 20.0, double fHi = 20000.0, int pointsPerOctave = 48);

// numpy.interp: linear interpolation, clamped to the end values.
double interp (double x, const std::vector<double>& xp, const std::vector<double>& fp);

// 1/N-octave smoothing of a power spectrum onto outFreqs. fraction <= 0 means
// no smoothing (piecewise-constant lookup). Zero-weight bins are ignored, even
// if they hold NaN. `freqs` must be a uniform FFT grid starting at 0.
std::vector<double> smoothPower (const std::vector<double>& freqs, const std::vector<double>& power,
                                 double fraction, const std::vector<double>& outFreqs,
                                 const std::vector<double>* weights = nullptr);

// Box-average a power spectrum onto another uniform FFT grid (both start at 0).
std::vector<double> rebinPower (const std::vector<double>& freqs, const std::vector<double>& power,
                                const std::vector<double>& outFreqs);

double toDb (double power, double floor = 1e-20);
std::vector<double> toDb (const std::vector<double>& power, double floor = 1e-20);

// Octave bands used for grading (IEC nominal centres 31.5 Hz ... 16 kHz).
inline constexpr std::array<double, 10> octaveCenters { 31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
inline constexpr std::array<const char*, 10> octaveNominal { "31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

std::pair<double, double> octaveEdges (double center);

// Two significant figures: 88 Hz, 180 Hz, 1.4 kHz, 11 kHz.
std::string formatHz (double f);

struct BandSum
{
    double sum = 0.0;
    int count = 0;
};

BandSum bandSum (const std::vector<double>& freqs, const std::vector<double>& values, double lo, double hi);

double nanMean (const std::vector<double>& v);
} // namespace roomeq
