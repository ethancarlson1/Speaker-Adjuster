#include "roomeq/alignment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

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

const char* confidenceLabel (Confidence c)
{
    switch (c)
    {
        case Confidence::high: return "high";
        case Confidence::medium: return "medium";
        case Confidence::low: return "low";
    }
    return "low";
}

Arrival estimateArrival (const Capture& c, double separationMs, const WindowConfig& window)
{
    Arrival a;
    if (c.delaysMs.empty())
        return { std::nan (""), Confidence::low, { "no delay was measured" } };
    a.ms = c.delaysMs.front();
    auto& conf = a.confidence;
    auto& reasons = a.reasons;

    if (c.ir.size() > 2)
    {
        std::vector<double> ir (c.ir.size());
        for (std::size_t i = 0; i < ir.size(); ++i)
            ir[i] = std::abs (c.ir[i]);
        const auto nPre = irWindow (window, c.fs).nPre;
        const auto strongest = static_cast<std::size_t> (std::max_element (ir.begin(), ir.end()) - ir.begin());
        // The first peak within 6 dB of the strongest: the first sample over the
        // threshold, then up its slope to the top.
        const auto threshold = ir[strongest] * std::pow (10.0, -firstArrivalDb / 20.0);
        std::size_t pk = 0;
        while (pk < ir.size() && ! (ir[pk] >= threshold))
            ++pk;
        while (pk + 1 < ir.size() && ir[pk + 1] > ir[pk])
            ++pk;
        // Below a sample: a parabola through the peak and its neighbours.
        auto frac = 0.0;
        if (pk > 0 && pk + 1 < ir.size())
        {
            const auto y0 = ir[pk - 1], y1 = ir[pk], y2 = ir[pk + 1];
            const auto denom = y0 - 2.0 * y1 + y2;
            frac = denom != 0.0 ? std::clamp (0.5 * (y0 - y2) / denom, -0.5, 0.5) : 0.0;
        }
        a.ms += 1000.0 * (static_cast<double> (pk) - static_cast<double> (nPre) + frac) / c.fs;

        // Repeats.
        const auto [lo, hi] = std::minmax_element (c.delaysMs.begin(), c.delaysMs.end());
        const auto spread = *hi - *lo;
        if (spread > 0.25)
        {
            conf = worse (conf, Confidence::low);
            reasons.push_back ("the repeat sweeps disagree about the arrival by " + format ("%.2f", spread) + " ms (something moved)");
        }
        else if (spread > 0.05)
        {
            conf = worse (conf, Confidence::medium);
            reasons.push_back ("the repeat sweeps differ by " + format ("%.2f", spread) + " ms");
        }

        // The direct sound: weaker than a later arrival, or something earlier nearly counted.
        const auto belowDb = 20.0 * std::log10 (ir[strongest] / std::max (ir[pk], 1e-30));
        if (belowDb > 3.0)
        {
            conf = worse (conf, Confidence::medium);
            reasons.push_back ("a later arrival is " + format ("%.1f", belowDb) + " dB stronger than the direct sound, "
                               + format ("%.1f", 1000.0 * (static_cast<double> (strongest) - static_cast<double> (pk)) / c.fs)
                               + " ms after it (the direct sound may be partly blocked)");
        }
        const auto sep = static_cast<std::size_t> (std::llround (separationMs * 1e-3 * c.fs));
        const auto beforeEnd = pk > sep ? pk - sep : 0;
        if (beforeEnd > 0)
        {
            const auto k = static_cast<std::size_t> (std::max_element (ir.begin(), ir.begin() + static_cast<long> (beforeEnd)) - ir.begin());
            const auto downDb = 20.0 * std::log10 (ir[strongest] / std::max (ir[k], 1e-30));
            if (downDb < earlierWarningDb)
            {
                conf = worse (conf, Confidence::medium);
                reasons.push_back ("an earlier arrival " + format ("%.1f", downDb) + " dB down, "
                                   + format ("%.1f", 1000.0 * (static_cast<double> (pk) - static_cast<double> (k)) / c.fs)
                                   + " ms before: if that's the direct sound, it arrives that much earlier");
            }
        }
    }
    else
    {
        conf = worse (conf, Confidence::medium);
        reasons.push_back ("measured from pink noise or music: to the nearest sample, with no impulse response to check");
    }

    // Noise in the bands that set the arrival: the three highest graded ones.
    std::vector<const BandResult*> graded;
    for (const auto& b : c.grade.bands)
        if (! b.outOfRange)
            graded.push_back (&b);
    if (! graded.empty())
    {
        auto worst = std::numeric_limits<double>::infinity();
        for (auto i = graded.size() > 3 ? graded.size() - 3 : 0; i < graded.size(); ++i)
            worst = std::min (worst, graded[i]->snrDb);
        if (worst < 10.0)
        {
            conf = worse (conf, Confidence::low);
            reasons.push_back ("noisy: " + format ("%.0f", worst) + " dB SNR where the arrival is set");
        }
        else if (worst < 20.0)
        {
            conf = worse (conf, Confidence::medium);
            reasons.push_back (format ("%.0f", worst) + " dB SNR where the arrival is set");
        }
    }
    else
    {
        conf = worse (conf, Confidence::low);
        reasons.push_back ("no band was heard clearly");
    }
    return a;
}

double equivalentDistanceM (double arrivalMs, double latencyMs)
{
    return (arrivalMs - latencyMs) * 1e-3 * speedOfSound;
}

DelaySuggestion suggestZoneDelay (double mainMs, double zoneMs, double mainZoneDelayMs)
{
    const auto diff = mainMs + mainZoneDelayMs - zoneMs;
    if (diff >= 0.0)
        return { diff, diff, {} };
    return { 0.0, diff, "this zone already arrives " + format ("%.2f", -diff)
                            + " ms after the main system here: delay the main system instead, or measure where the two overlap" };
}
} // namespace roomeq
