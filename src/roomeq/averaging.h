#pragma once

// Multi-position power averaging, usable-range detection, quick mode, and the
// session summary the UI draws. Port of prototype/roomeq/averaging.py.

#include "roomeq/grading.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace roomeq
{
struct Capture;

// Per-bin weight in [0, 1]: 0 in bands graded redo, 1 elsewhere, interpolated
// in log frequency between octave centres.
std::vector<double> bandMaskWeights (const std::vector<double>& freqs, const CaptureGrade& grade);

// Mean level (dB, 1/3-octave smoothed, log-spaced) over [lo, hi].
double levelOffsetDb (const std::vector<double>& freqs, const std::vector<double>& power,
                      double lo = 250.0, double hi = 4000.0, const std::vector<double>* weights = nullptr);

struct AverageResult
{
    std::vector<double> freqs;
    std::vector<double> power;       // weighted power average (NaN where no capture has weight)
    std::vector<double> weight;      // total weight per bin
    std::vector<double> offsetsDb;   // level alignment applied to each capture
};

AverageResult powerAverage (const std::vector<double>& freqs, const std::vector<std::vector<double>>& powers,
                            const std::vector<std::vector<double>>& weights, bool alignLevels = true,
                            double lo = 250.0, double hi = 4000.0);

// PA usable range: walk down/up from startHz until the (heavily smoothed)
// response falls dropDb below the passband mean.
std::pair<double, double> usableRange (const std::vector<double>& freqsLog, const std::vector<double>& levelDb,
                                       double refLo = 250.0, double refHi = 4000.0, double dropDb = 10.0,
                                       double startHz = 1000.0);

double targetLevelDb (const std::vector<double>& freqsLog, const std::vector<double>& levelDb,
                      double lo = 250.0, double hi = 4000.0);

struct QuickModePolicy
{
    int smoothingFraction = 6;               // N of 1/N octave actually used for the average
    double strength = 1.0;                   // 0..1 scale on the correction (Phase 2)
    std::optional<double> maxCorrectionDb;   // empty = no extra cap
};

QuickModePolicy quickModePolicy (int nGood, int userFraction);

// Everything the response graph needs for a set of captures.
struct SessionSummary
{
    std::vector<double> freqs;               // linear grid of the average
    std::vector<double> power, weight;
    std::vector<double> offsetsDb;           // per capture (all, including excluded)
    int nGood = 0;                           // included captures not graded redo
    QuickModePolicy policy;
    std::vector<double> grid;                // log display grid
    std::vector<double> averageDb;           // smoothed at policy.smoothingFraction
    std::vector<std::vector<double>> positionDb;   // per capture, level-aligned, same smoothing
    std::pair<double, double> usable { 0.0, 0.0 };
    double targetDb = 0.0;
};

// Captures on a different sample rate are rebinned onto the first included
// capture's grid. Returns nullopt if every capture is excluded.
std::optional<SessionSummary> summarizeSession (const std::vector<std::shared_ptr<const Capture>>& captures,
                                                int userFraction, const std::vector<double>& grid);
} // namespace roomeq
