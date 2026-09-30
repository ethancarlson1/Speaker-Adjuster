#include "roomeq/quality.h"

#include "roomeq/averaging.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace roomeq
{
namespace
{
std::string format (const char* fmt, double value)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), fmt, value);
    return buf;
}

Confidence worse (Confidence a, Confidence b)
{
    return static_cast<int> (a) >= static_cast<int> (b) ? a : b;
}
} // namespace

const char* ratingLabel (Rating r)
{
    switch (r)
    {
        case Rating::excellent: return "Excellent";
        case Rating::good: return "Good";
        case Rating::fair: return "Fair";
        case Rating::poor: return "Poor";
    }
    return "Poor";
}

Rating rate (double value, const RatingLimits& limits, bool higherIsBetter)
{
    const auto meets = [&] (double limit) { return higherIsBetter ? value >= limit : value <= limit; };
    if (meets (limits.excellent))
        return Rating::excellent;
    if (meets (limits.good))
        return Rating::good;
    if (meets (limits.fair))
        return Rating::fair;
    return Rating::poor;
}

std::pair<double, double> captureUsableRange (const Capture& c, double refLo, double refHi)
{
    const auto grid = logFreqGrid (20.0, 20000.0, 48);
    auto weights = bandMaskWeights (c.freqs, c.grade);
    for (std::size_t k = 0; k < weights.size() && k < c.weight.size(); ++k)
        weights[k] *= c.weight[k];
    const auto level = toDb (smoothPower (c.freqs, c.power, 1, grid, &weights));
    auto any = false;
    for (std::size_t i = 0; i < grid.size(); ++i)
        any = any || (grid[i] >= refLo && grid[i] <= refHi && std::isfinite (level[i]));
    if (! any)
        return { std::nan (""), std::nan ("") };
    return usableRange (grid, level, refLo, refHi);
}

MeasurementQuality measurementQuality (const Capture& c, double refLo, double refHi)
{
    MeasurementQuality q;
    q.usable = captureUsableRange (c, refLo, refHi);
    std::vector<const BandResult*> graded;
    for (const auto& b : c.grade.bands)
        if (! b.outOfRange)
            graded.push_back (&b);
    if (graded.empty())
    {
        q.snrDb = std::nan ("");
        q.confidence = Confidence::low;
        q.reasons = { "no band was heard clearly" };
        return q;
    }

    const BandResult* worst = graded.front();
    for (const auto* b : graded)
        if (b->snrDb < worst->snrDb)
            worst = b;
    q.snrDb = worst->snrDb;
    q.snrBand = worst->name;
    q.snr = rate (q.snrDb, snrLimitsDb);

    const BandResult* leastCoherent = nullptr;
    const BandResult* leastRepeatable = nullptr;
    for (const auto* b : graded)
    {
        if (b->coherence && (leastCoherent == nullptr || *b->coherence < *leastCoherent->coherence))
            leastCoherent = b;
        if (b->excessSpreadDb && (leastRepeatable == nullptr || *b->excessSpreadDb > *leastRepeatable->excessSpreadDb))
            leastRepeatable = b;
    }
    if (leastCoherent != nullptr)
    {
        q.coherence = leastCoherent->coherence;
        q.coherenceBand = leastCoherent->name;
        q.coherenceRating = rate (*q.coherence, coherenceLimits);
    }
    if (leastRepeatable != nullptr)
    {
        q.repeatDb = leastRepeatable->excessSpreadDb;
        q.repeatBand = leastRepeatable->name;
        q.repeatability = rate (*q.repeatDb, repeatLimitsDb, false);
    }

    const auto judge = [&q] (const char* name, std::optional<Rating> rating, const std::string& detail)
    {
        if (rating == Rating::poor)
        {
            q.confidence = worse (q.confidence, Confidence::low);
            q.reasons.push_back (std::string (name) + " poor: " + detail);
        }
        else if (rating == Rating::fair)
        {
            q.confidence = worse (q.confidence, Confidence::medium);
            q.reasons.push_back (std::string (name) + " fair: " + detail);
        }
    };
    judge ("signal-to-noise", q.snr, format ("%.0f", q.snrDb) + " dB at " + q.snrBand + " Hz");
    judge ("coherence", q.coherenceRating, q.coherence ? format ("%.2f", *q.coherence) + " at " + q.coherenceBand + " Hz" : "");
    judge ("repeatability", q.repeatability,
           q.repeatDb ? "repeats " + format ("%.1f", *q.repeatDb) + " dB apart at " + q.repeatBand + " Hz" : "");
    if (c.grade.overall == Grade::redo)
    {
        q.confidence = worse (q.confidence, Confidence::low);
        q.reasons.push_back ("graded redo");
    }
    else if (c.grade.overall == Grade::marginal)
    {
        q.confidence = worse (q.confidence, Confidence::medium);
        if (q.reasons.empty())
            q.reasons.push_back ("graded marginal");
    }
    return q;
}
} // namespace roomeq
