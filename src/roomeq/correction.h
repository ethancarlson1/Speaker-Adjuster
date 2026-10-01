#pragma once

// Fit a conservative minimum-phase correction to the averaged measurement.
// Port of prototype/roomeq/correction.py (same algorithm, step for step):
//
// 1. Average at the session's smoothing, its 1-octave trend and a 1/6-octave
//    version, on a 24-points-per-octave grid.
// 2. Fit range: the PA's -6 dB points on the trend, intersected with the
//    user's frequency range. Nothing is boosted outside it.
// 3. Nulls: the average dips > 6 dB below its trend, or the positions (1/3
//    octave, level-aligned) disagree by > 6 dB. Cut there, never boost.
// 4. Wanted correction = target - average, clipped to the limits.
// 5. Bands added greedily (bell on the largest-area residual, or a shelf),
//    all refined together by a bounded Levenberg-Marquardt fit.
// 6. Quick mode: limits raised to cap/strength, gains scaled by strength.

#include "roomeq/averaging.h"
#include "roomeq/filters.h"
#include "roomeq/targets.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace roomeq
{
struct Capture;

struct CorrectionConfig
{
    int maxBands = 10;
    double maxCutDb = 12.0;
    double maxBoostDb = 3.0;
    double rangeLoHz = 20.0;              // user frequency range, intersected with the PA's
    double rangeHiHz = 20000.0;
    double rolloffDb = 6.0;               // PA range edge: this far below the reference band's mean
    double refBandLoHz = 250.0;           // the zone's reference band (subs: 40-100 Hz): the range
    double refBandHiHz = 4000.0;          // edges are found from it and the target is placed on it
    double boostMinOctaves = 1.0;
    double cutMinOctavesLow = 1.0 / 3.0;  // below cutSplitLoHz (room modes that survive averaging)
    double cutMinOctavesHigh = 2.0 / 3.0; // above cutSplitHiHz
    double cutSplitLoHz = 250.0;          // log-interpolated between the two widths
    double cutSplitHiHz = 360.0;
    double maxOctaves = 3.0;              // widest bell
    double nullDipDb = 6.0;
    double nullSpreadDb = 6.0;
    double nullMarginOctaves = 1.0 / 6.0;
    int pointsPerOctave = 24;
    double outsideWeight = 0.05;          // slight pull towards 0 dB outside the fit range
    double penalty = 100.0;               // weight on exceeding the limits
    double minGainDb = 0.5;               // smaller bands are dropped
    double minImprovement = 0.02;         // a new band must cut the fit cost by this fraction
    double stopRmsDb = 0.5;               // stop adding bands below this in-range RMS error
};

// Everything the band fit needs, on the fit grid.
struct FitProblem
{
    std::vector<double> freqs;
    double fs = 48000.0;
    std::vector<double> desired;    // dB, clipped; 0 outside the range
    std::vector<double> weight;     // per point (0 where the average is missing)
    std::vector<double> upper;      // max allowed correction per point (dB)
    std::vector<double> lower;      // min allowed correction per point (dB, negative)
    double fLo = 20.0, fHi = 20000.0;
    double maxCut = 12.0, maxBoost = 3.0;
    CorrectionConfig cfg;
};

struct CorrectionResult
{
    std::vector<double> grid;
    std::vector<double> averageDb;     // the average the fit worked from
    std::vector<double> trendDb;       // 1-octave trend
    std::vector<double> targetDb;      // anchored target
    std::vector<double> desiredDb;     // clipped target - average (0 outside the range)
    std::vector<double> upperDb, lowerDb;
    std::vector<bool> nullMask;
    std::pair<double, double> fitRange { 20.0, 20000.0 };
    std::vector<Band> fitted;          // full strength, within the (cap-adjusted) limits
    std::vector<Band> bands;           // scaled by the quick-mode strength: what the audio path gets
    double strength = 1.0;
    std::vector<double> correctionDb;  // response of `bands`
    std::vector<double> predictedDb;   // average + correction
    double rmsErrorDb = 0.0;           // predicted vs target, in range, outside nulls (NaN if none)
    // What the decisions were made from (the system summary explains them):
    std::pair<double, double> paRange;  // the PA's own -6 dB points, before the user's range
    std::vector<double> dipDb;          // 1/6-octave average - 1-octave trend (below -nullDipDb: a null)
    std::vector<double> spreadDb;       // positions' max - min (1/3 octave, level-aligned); NaN with fewer than 2
    double maxCutDb = 0.0, maxBoostDb = 0.0;   // the fit's limits (quick mode: raised to cap / strength)
};

double minCutOctaves (double freq, const CorrectionConfig& cfg);
double maxQ (double freq, double gainDb, const CorrectionConfig& cfg);

std::vector<Band> fitBands (const FitProblem& prob);

// Included, non-redo captures, level-aligned and smoothed to 1/fraction octave on grid, redo bands masked.
std::vector<std::vector<double>> levelAlignedPositions (const std::vector<std::shared_ptr<const Capture>>& captures,
                                                        const SessionSummary& summary, double fraction,
                                                        const std::vector<double>& grid);

// Max - min across positions per point; NaN where fewer than two have a value.
std::vector<double> positionSpread (std::size_t n, const std::vector<std::vector<double>>& positions);

std::vector<bool> detectNulls (const std::vector<double>& grid, const std::vector<double>& fineDb,
                               const std::vector<double>& trendDb, const std::vector<std::vector<double>>& positions,
                               const CorrectionConfig& cfg);

CorrectionResult designCorrection (const std::vector<std::shared_ptr<const Capture>>& captures,
                                   const SessionSummary& summary, const TargetCurve& target, double fs,
                                   const CorrectionConfig& cfg = {});
} // namespace roomeq
