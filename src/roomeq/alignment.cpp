#include "roomeq/alignment.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <tuple>

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

LowResponse lowResponse (const Capture& c)
{
    return { c.low, 0.0 };
}

double bandSnr (const std::vector<BandResult>& bands, double lo, double hi)
{
    auto worst = std::numeric_limits<double>::infinity();
    for (const auto& b : bands)
        if (! b.outOfRange && b.hi > lo && b.lo < hi)
            worst = std::min (worst, b.snrDb);
    return worst;
}

namespace
{
constexpr double pi = 3.14159265358979323846;

// 1/6 octave (+-2 points at 24 per octave) power average, in dB.
std::vector<double> smoothedDb (const std::vector<cplx>& h)
{
    std::vector<double> out (h.size());
    for (std::size_t i = 0; i < h.size(); ++i)
    {
        const auto lo = i >= 2 ? i - 2 : 0;
        const auto hi = std::min (i + 3, h.size());
        auto sum = 0.0;
        for (auto k = lo; k < hi; ++k)
            sum += std::norm (h[k]);
        out[i] = 10.0 * std::log10 (std::max (sum / static_cast<double> (hi - lo), 1e-30));
    }
    return out;
}

struct Candidate
{
    double level, delay;
    bool invert;
};

struct Crossover
{
    std::size_t lo = 0, crossing = 0, hi = 0;   // on lfGrid()
    std::string none;                           // why there's no crossover (empty when there is one)
};

// Only the magnitudes count, so delays and polarity don't move it.
Crossover findCrossover (const std::vector<cplx>& hm, const std::vector<cplx>& hs)
{
    const auto& f = lfGrid();
    const auto dm = smoothedDb (hm), ds = smoothedDb (hs);
    std::vector<double> d (f.size());
    for (std::size_t i = 0; i < f.size(); ++i)
        d[i] = dm[i] - ds[i];
    std::vector<std::size_t> inside;
    for (std::size_t i = 0; i < f.size(); ++i)
        if (f[i] >= 30.0 && f[i] <= 300.0)
            inside.push_back (i);
    Crossover x;
    auto found = false;
    for (std::size_t j = 1; j < inside.size() && ! found; ++j)
        if (d[inside[j] - 1] < 0.0 && d[inside[j]] >= 0.0)
        {
            x.crossing = inside[j];
            found = true;
        }
    if (! found)
    {
        const auto allBelow = std::all_of (inside.begin(), inside.end(), [&] (std::size_t i) { return d[i] < 0.0; });
        const auto allAbove = std::all_of (inside.begin(), inside.end(), [&] (std::size_t i) { return d[i] >= 0.0; });
        x.none = allBelow   ? "the sub is louder than the mains all the way up to 300 Hz"
                 : allAbove ? "the mains are louder than the sub all the way down to 30 Hz"
                            : "the mains and the sub never cross over between 30 and 300 Hz";
        return x;
    }
    x.lo = x.hi = x.crossing;
    while (x.lo > inside.front() && std::abs (d[x.lo - 1]) <= subRegionDb)
        --x.lo;
    while (x.hi < inside.back() && std::abs (d[x.hi + 1]) <= subRegionDb)
        ++x.hi;
    return x;
}

bool usable (const LowResponse& r) { return r.h.size() == lfGrid().size(); }
} // namespace

std::optional<std::pair<double, double>> crossoverRegion (const LowResponse& main, const LowResponse& sub)
{
    if (! usable (main) || ! usable (sub))
        return std::nullopt;
    const auto x = findCrossover (main.h, sub.h);
    if (! x.none.empty())
        return std::nullopt;
    return std::make_pair (lfGrid()[x.lo], lfGrid()[x.hi]);
}

SubAlignment alignSub (const LowResponse& main, const LowResponse& sub, const SubSettings& s)
{
    SubAlignment out;
    const auto& f = lfGrid();
    if (! usable (main) || ! usable (sub))
    {
        out.note = "Both need a sweep measurement (pink noise and music don't keep the phase this needs).";
        return out;
    }
    std::vector<cplx> hm (f.size()), hs (f.size());
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        const auto w = 2.0 * pi * f[i] * 1e-3;
        hm[i] = main.h[i] * std::polar (s.mainInvert ? -1.0 : 1.0, -w * (main.refMs + s.mainDelayMs));
        hs[i] = sub.h[i] * std::polar (1.0, -w * sub.refMs);
    }

    // The crossover: where they cross (sub louder below, mains louder above), and within 10 dB around it.
    const auto x = findCrossover (hm, hs);
    if (! x.none.empty())
    {
        out.note = "No crossover to align: " + x.none + ". Check both were measured at the same spot.";
        return out;
    }
    const auto lo = x.lo, hi = x.hi;

    // Mean summed level (dB) over the region for a sub delay and polarity.
    const auto summed = [&] (double delayMs, bool invert)
    {
        auto total = 0.0;
        for (auto i = lo; i <= hi; ++i)
        {
            const auto v = hm[i] + hs[i] * std::polar (invert ? -1.0 : 1.0, -2.0 * pi * f[i] * delayMs * 1e-3);
            total += 20.0 * std::log10 (std::max (std::abs (v), 1e-30));
        }
        return total / static_cast<double> (hi - lo + 1);
    };

    const auto steps = static_cast<int> (std::lround (subSearchMs * 100.0));   // 0.01 ms steps
    std::vector<Candidate> candidates;                                         // local maxima
    for (const auto invert : { false, true })
    {
        std::vector<double> delays, level;
        for (auto i = -steps; i <= steps; ++i)
        {
            delays.push_back (static_cast<double> (i) / 100.0);
            level.push_back (summed (delays.back(), invert));
        }
        for (std::size_t i = 0; i < level.size(); ++i)
        {
            const auto left = i > 0 ? level[i - 1] : -std::numeric_limits<double>::infinity();
            const auto right = i + 1 < level.size() ? level[i + 1] : -std::numeric_limits<double>::infinity();
            if (level[i] >= left && level[i] > right)
                candidates.push_back ({ level[i], delays[i], invert });
        }
    }
    auto bestLevel = -std::numeric_limits<double>::infinity();
    for (const auto& c : candidates)
        bestLevel = std::max (bestLevel, c.level);
    // Among the near-best: not a negative delay, then normal polarity, then the smallest delay.
    const auto key = [] (const Candidate& c) { return std::make_tuple (c.delay < 0.0, c.invert, std::abs (c.delay)); };
    const Candidate* pick = nullptr;
    for (const auto& c : candidates)
        if (c.level >= bestLevel - subNearDb && (pick == nullptr || key (c) < key (*pick)))
            pick = &c;

    const auto now = summed (s.subDelayMs, s.subInvert);
    auto ideal = 0.0;
    for (auto i = lo; i <= hi; ++i)
        ideal += 20.0 * std::log10 (std::abs (hm[i]) + std::abs (hs[i]));
    ideal /= static_cast<double> (hi - lo + 1);

    out.ok = true;
    out.regionLoHz = f[lo];
    out.regionHiHz = f[hi];
    out.crossingHz = f[x.crossing];
    out.delayMs = pick->delay;
    out.invert = pick->invert;
    out.summedDb = pick->level;
    out.improvementDb = pick->level - now;
    out.efficiencyDb = pick->level - ideal;

    // How far to trust it.
    const auto snr = std::min (s.mainSnrDb, s.subSnrDb);
    if (snr < 10.0)
    {
        out.confidence = worse (out.confidence, Confidence::low);
        out.reasons.push_back ("noisy: " + format ("%.0f", snr) + " dB SNR over the crossover");
    }
    else if (snr < 20.0)
    {
        out.confidence = worse (out.confidence, Confidence::medium);
        out.reasons.push_back (format ("%.0f", snr) + " dB SNR over the crossover");
    }
    if (std::log2 (f[hi] / f[lo]) < 1.0 / 3.0)
    {
        out.confidence = worse (out.confidence, Confidence::medium);
        out.reasons.push_back ("the mains and the sub overlap over less than a third of an octave");
    }
    // A delay a whole period away summing about as well: the overlap doesn't pin the timing down.
    const auto periodMs = 1000.0 / out.crossingHz;
    const Candidate* alt = nullptr;
    for (const auto& c : candidates)
        if (c.invert == pick->invert && std::abs (c.delay - pick->delay) >= 0.75 * periodMs && (alt == nullptr || c.level > alt->level))
            alt = &c;
    if (alt != nullptr && pick->level - alt->level < 0.3)
    {
        out.confidence = worse (out.confidence, Confidence::medium);
        out.reasons.push_back (format ("%.2f", alt->delay) + " ms, a period away, sums about as well ("
                               + format ("%.1f", pick->level - alt->level) + " dB less)");
    }
    if (pick->delay < 0.0)
        out.note = "The sub arrives " + format ("%.2f", -pick->delay) + " ms late here: delay the mains by "
                   + format ("%.2f", -pick->delay) + " ms instead (and anything lined up with them)";
    return out;
}
} // namespace roomeq
