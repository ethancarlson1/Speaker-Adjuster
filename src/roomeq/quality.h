#pragma once

// What the analysis found, in plain words: a capture's measurement quality.
// Port of prototype/roomeq/quality.py; see there for the rules.

#include "roomeq/alignment.h"
#include "roomeq/capture.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace roomeq
{
enum class Rating
{
    excellent = 0,
    good,
    fair,
    poor
};
const char* ratingLabel (Rating r);   // "Excellent", "Good", "Fair", "Poor"

struct RatingLimits
{
    double excellent, good, fair;
};
constexpr RatingLimits snrLimitsDb { 30.0, 20.0, 10.0 };        // at or above; good and fair are the grading's pass and marginal
constexpr RatingLimits coherenceLimits { 0.95, 0.85, 0.6 };     // at or above
constexpr RatingLimits repeatLimitsDb { 0.5, 1.0, 3.0 };        // excess repeat spread at or below

Rating rate (double value, const RatingLimits& limits, bool higherIsBetter = true);

struct MeasurementQuality
{
    double snrDb = 0.0;                     // worst graded band
    std::string snrBand;
    Rating snr = Rating::poor;
    std::optional<double> coherence;        // worst graded band (music and noise only)
    std::string coherenceBand;
    std::optional<Rating> coherenceRating;
    std::optional<double> repeatDb;         // worst excess repeat spread (two or more sweeps only)
    std::string repeatBand;
    std::optional<Rating> repeatability;
    std::pair<double, double> usable;       // NaN when the whole reference band is redo
    Confidence confidence = Confidence::high;
    std::vector<std::string> reasons;
};

// Where the capture is within 10 dB of its reference band, 1-octave smoothed (redo bands left out).
std::pair<double, double> captureUsableRange (const Capture& c, double refLo, double refHi);

MeasurementQuality measurementQuality (const Capture& c, double refLo = 250.0, double refHi = 4000.0);
} // namespace roomeq
