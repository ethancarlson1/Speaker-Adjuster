#pragma once

// Target curves: flat, house, speech, and user-drawn ones, as (Hz, dB) points
// interpolated with a shape-preserving cubic (PCHIP) in log frequency and
// held flat beyond the end points. Port of prototype/roomeq/targets.py.

#include <string>
#include <utility>
#include <vector>

namespace roomeq
{
struct TargetCurve
{
    std::string name;
    std::vector<std::pair<double, double>> points;   // (Hz, dB), strictly increasing in Hz

    std::vector<double> db (const std::vector<double>& freqs) const;
    double db (double freq) const;
    bool operator== (const TargetCurve& o) const;   // exact (defined out of line: -Wfloat-equal)
    bool operator!= (const TargetCurve& o) const { return ! (*this == o); }
};

const TargetCurve& flatTarget();
const TargetCurve& houseTarget();
const TargetCurve& speechTarget();
const std::vector<TargetCurve>& targetPresets();   // flat, house, speech

// Fritsch-Carlson monotone cubic through `points` in log2(Hz) (scipy's pchip).
std::vector<double> pchipLog (const std::vector<std::pair<double, double>>& points, const std::vector<double>& freqs);

// Level that places `target` on the measured average (mean difference over [lo, hi]).
double anchorOffsetDb (const std::vector<double>& freqsLog, const std::vector<double>& levelDb,
                       const TargetCurve& target, double lo = 250.0, double hi = 4000.0);

// Points sorted, de-duplicated (>= 1/48 octave apart) and clamped to 20 Hz-20 kHz, +-24 dB.
std::vector<std::pair<double, double>> sanitizeTargetPoints (std::vector<std::pair<double, double>> points);
} // namespace roomeq
