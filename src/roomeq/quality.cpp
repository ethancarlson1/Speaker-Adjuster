#include "roomeq/quality.h"

#include "roomeq/averaging.h"

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

// Contiguous true runs as (first, last) index pairs.
std::vector<std::pair<std::size_t, std::size_t>> runs (const std::vector<bool>& mask)
{
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::optional<std::size_t> start;
    for (std::size_t i = 0; i < mask.size(); ++i)
    {
        if (mask[i] && ! start)
            start = i;
        else if (! mask[i] && start)
        {
            out.emplace_back (*start, i - 1);
            start.reset();
        }
    }
    if (start)
        out.emplace_back (*start, mask.size() - 1);
    return out;
}

std::string names (const std::vector<std::string>& n)
{
    if (n.size() == 1)
        return n.front();
    std::string out;
    for (std::size_t i = 0; i + 1 < n.size(); ++i)
        out += (i > 0 ? ", " : "") + n[i];
    return out + " and " + n.back();
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

std::vector<Explanation> explainCorrection (const std::vector<std::shared_ptr<const Capture>>& captures,
                                            const SessionSummary& summary, const CorrectionResult& result,
                                            const CorrectionConfig& cfg)
{
    std::vector<Explanation> out;
    const auto& grid = result.grid;
    const auto n = grid.size();
    const auto [lo, hi] = result.fitRange;
    std::vector<bool> inside (n), finite (n);
    std::vector<double> want (n);   // the correction each point asks for, before limits
    for (std::size_t i = 0; i < n; ++i)
    {
        inside[i] = grid[i] >= lo && grid[i] <= hi;
        finite[i] = std::isfinite (result.averageDb[i]);
        want[i] = result.targetDb[i] - result.averageDb[i];
    }
    const auto& policy = summary.policy;

    if (policy.strength < 1.0)
    {
        const auto good = summary.nGood;
        const auto start = good == 0 ? std::string ("No position graded better than redo")
                                     : "Only " + std::to_string (good) + " good position" + (good != 1 ? "s" : "");
        out.push_back ({ "positions", 0.0, 0.0,
                         start + ": the fit smooths to 1/" + std::to_string (policy.smoothingFraction) + " octave and is held to "
                             + format ("%.0f", policy.strength * 100.0) + "% strength, at most "
                             + format ("%g", policy.maxCorrectionDb.value_or (0.0)) + " dB. Measure "
                             + std::to_string (3 - good) + " more to correct fully." });
    }

    const auto [paLo, paHi] = result.paRange;   // (the grid's ends when the PA never drops that far)
    if (lo == paLo && paLo > cfg.rangeLoHz && paLo > grid.front())
        out.push_back ({ "bandwidth", cfg.rangeLoHz, lo,
                         "Not correcting below " + formatHz (lo) + ": the system is more than " + format ("%g", cfg.rolloffDb)
                             + " dB down there (its low-frequency limit), and boosting it costs headroom for little gain." });
    if (hi == paHi && paHi < cfg.rangeHiHz && paHi < grid.back())
        out.push_back ({ "bandwidth", hi, cfg.rangeHiHz,
                         "Not correcting above " + formatHz (hi) + ": the system is more than " + format ("%g", cfg.rolloffDb)
                             + " dB down there (its high-frequency limit), and boosting it costs headroom for little gain." });

    // Nulls that stopped a boost the target asked for.
    std::vector<bool> nullInside (n);
    for (std::size_t i = 0; i < n; ++i)
        nullInside[i] = result.nullMask[i] && inside[i];
    for (const auto& [a, b] : runs (nullInside))
    {
        auto best = a;
        auto bestWant = -std::numeric_limits<double>::infinity();
        auto dipMin = std::numeric_limits<double>::infinity();
        auto spreadMax = -std::numeric_limits<double>::infinity();
        auto isDip = false;
        for (auto i = a; i <= b; ++i)
        {
            const auto w = finite[i] ? want[i] : -std::numeric_limits<double>::infinity();
            if (w > bestWant)
            {
                bestWant = w;
                best = i;
            }
            if (std::isfinite (result.dipDb[i]))
                dipMin = std::min (dipMin, result.dipDb[i]);
            isDip = isDip || result.dipDb[i] < -cfg.nullDipDb;
            if (std::isfinite (result.spreadDb[i]))
                spreadMax = std::max (spreadMax, result.spreadDb[i]);
        }
        if (! (bestWant > cfg.minGainDb))
            continue;
        const auto disagree = std::isfinite (spreadMax) && spreadMax > cfg.nullSpreadDb;
        if (isDip)
        {
            auto text = "Not boosting the dip at " + formatHz (grid[best]) + " (" + format ("%.0f", -dipMin)
                        + " dB below the trend): it looks like a cancellation that moves from seat to seat, and boosting it wastes power.";
            if (disagree)
                text += " The positions also disagree there, by up to " + format ("%.0f", spreadMax) + " dB.";
            out.push_back ({ "null", grid[a], grid[b], text });
        }
        else if (disagree)
        {
            out.push_back ({ "disagreement", grid[a], grid[b],
                             "Not boosting around " + formatHz (grid[best]) + ": the positions disagree by up to "
                                 + format ("%.0f", spreadMax) + " dB there, so no single correction suits them all." });
        }
    }

    // Where the target asked for more than the limits allow.
    const auto capped = policy.maxCorrectionDb.has_value();
    for (const auto sign : { 1.0, -1.0 })
    {
        const auto limit = sign > 0 ? result.maxBoostDb : result.maxCutDb;
        std::vector<bool> over (n);
        for (std::size_t i = 0; i < n; ++i)
            over[i] = inside[i] && finite[i] && ! (sign > 0 && result.nullMask[i]) && sign * want[i] > limit + 0.5;
        for (const auto& [a, b] : runs (over))
        {
            auto i = a;
            for (auto k = a; k <= b; ++k)
                if (sign * want[k] > sign * want[i])
                    i = k;
            const auto applied = limit * result.strength;
            const auto user = sign > 0 ? cfg.maxBoostDb : cfg.maxCutDb;
            const std::string source = capped && applied < user - 1e-9 ? "the quick-mode cap" : sign > 0 ? "Max boost" : "Max cut";
            const auto text = sign > 0 ? "Boost held to +" + format ("%.1f", applied) + " dB at " + formatHz (grid[i]) + " (" + source
                                             + "): the response is " + format ("%.1f", want[i]) + " dB below the target there."
                                       : "Cut held to -" + format ("%.1f", applied) + " dB at " + formatHz (grid[i]) + " (" + source
                                             + "): the response is " + format ("%.1f", -want[i]) + " dB above the target there.";
            out.push_back ({ "limit", grid[a], grid[b], text });
        }
    }

    // Bands a position was left out of (graded redo), in the fit range.
    struct BandEntry
    {
        std::string name;
        double lo, hi;
        std::vector<std::pair<std::string, std::string>> entries;   // capture, cause
    };
    std::vector<BandEntry> byBand;
    std::size_t included = 0;
    for (const auto& c : captures)
    {
        if (c->excluded)
            continue;
        ++included;
        for (const auto& band : c->grade.bands)
        {
            if (band.outOfRange || band.grade() != Grade::redo || ! (band.hi > lo && band.lo < hi))
                continue;
            const std::string cause = band.snrGrade == Grade::redo ? (c->kind != "sweep" ? "coherence" : "noise") : "repeatability";
            auto it = std::find_if (byBand.begin(), byBand.end(), [&] (const BandEntry& e) { return e.name == band.name; });
            if (it == byBand.end())
            {
                byBand.push_back ({ band.name, band.lo, band.hi, {} });
                it = byBand.end() - 1;
            }
            it->entries.emplace_back (c->name, cause);
        }
    }
    const auto words = [] (const std::string& cause)
    {
        return cause == "noise" ? "too noisy" : cause == "coherence" ? "low coherence" : "the repeats disagreed";
    };
    for (const auto& band : byBand)
    {
        std::vector<std::string> causes, who;
        for (const auto& [name, cause] : band.entries)
        {
            if (std::find (causes.begin(), causes.end(), cause) == causes.end())
                causes.push_back (cause);
            who.push_back (name);
        }
        std::string why;
        for (std::size_t i = 0; i < causes.size(); ++i)
            why += (i > 0 ? "; " : "") + std::string (words (causes[i]));
        const auto kind = causes.size() == 1 ? causes.front() : std::string ("noise");
        const auto text = band.entries.size() == included
                              ? band.name + " Hz band: no position was clean enough there (" + why + "), so it isn't corrected."
                              : band.name + " Hz band: " + names (who) + " left out of the average there (" + why + ").";
        out.push_back ({ kind, band.lo, band.hi, text });
    }
    return out;
}

SystemSummary systemSummary (const std::vector<std::shared_ptr<const Capture>>& captures, const SessionSummary& summary,
                             const CorrectionResult& result, const CorrectionConfig& cfg)
{
    SystemSummary s;
    const auto& grid = result.grid;
    const auto n = grid.size();
    const auto [lo, hi] = result.fitRange;
    std::vector<bool> inside (n), judge (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        inside[i] = grid[i] >= lo && grid[i] <= hi;
        judge[i] = inside[i] && ! result.nullMask[i] && std::isfinite (result.averageDb[i]);
    }

    // Confidence, from the captures in the average.
    std::size_t count = 0, lowCount = 0;
    for (const auto& c : captures)
    {
        if (c->excluded)
            continue;
        ++count;
        const auto q = measurementQuality (*c, cfg.refBandLoHz, cfg.refBandHiHz);
        if (q.confidence != Confidence::high)
        {
            s.confidence = worse (s.confidence, Confidence::medium);
            s.confidenceReasons.push_back (c->name + ": " + confidenceLabel (q.confidence)
                                           + (q.reasons.empty() ? std::string() : " (" + q.reasons.front() + ")"));
        }
        if (q.confidence == Confidence::low)
            ++lowCount;
    }
    if (summary.nGood == 0 || lowCount * 2 > count)
        s.confidence = Confidence::low;
    if (summary.nGood < 3)
    {
        s.confidence = worse (s.confidence, Confidence::medium);
        s.confidenceReasons.insert (s.confidenceReasons.begin(), std::to_string (summary.nGood) + " good position"
                                                                     + (summary.nGood != 1 ? "s" : "") + " (3 or more for full confidence)");
    }

    // How much the positions differ.
    const auto positions = levelAlignedPositions (captures, summary, 3.0, grid);
    if (positions.size() >= 2)
    {
        auto total = 0.0;
        auto columns = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (! inside[i])
                continue;
            std::vector<double> v;
            for (const auto& p : positions)
                if (std::isfinite (p[i]))
                    v.push_back (p[i]);
            if (v.size() < 2)
                continue;
            auto mean = 0.0;
            for (const auto x : v)
                mean += x;
            mean /= static_cast<double> (v.size());
            auto var = 0.0;
            for (const auto x : v)
                var += (x - mean) * (x - mean);
            total += std::sqrt (var / static_cast<double> (v.size()));
            ++columns;
        }
        if (columns > 0)
        {
            s.variationDb = total / columns;
            s.coverage = rate (*s.variationDb, coverageLimitsDb, false);
        }
    }
    s.positions = summary.nGood;
    s.usable = summary.usable;

    // The largest broad issue.
    const auto broad = toDb (smoothPower (summary.freqs, summary.power, 3.0, grid, &summary.weight));
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto d = broad[i] - result.targetDb[i];
        if (judge[i] && std::isfinite (d) && (! s.issueDb || std::abs (d) > std::abs (*s.issueDb)))
        {
            s.issueDb = d;
            s.issueHz = grid[i];
        }
    }

    s.filters = static_cast<int> (result.bands.size());
    for (const auto v : result.correctionDb)
    {
        s.largestCutDb = std::min (s.largestCutDb, v);
        s.largestBoostDb = std::max (s.largestBoostDb, v);
    }
    auto sumSq = 0.0;
    auto judged = 0;
    for (std::size_t i = 0; i < n; ++i)
        if (judge[i])
        {
            const auto e = result.averageDb[i] - result.targetDb[i];
            sumSq += e * e;
            ++judged;
        }
    s.beforeDb = judged > 0 ? std::sqrt (sumSq / judged) : std::nan ("");
    s.afterDb = result.rmsErrorDb;
    s.explanations = explainCorrection (captures, summary, result, cfg);
    for (const auto& e : s.explanations)
        if (e.kind == "null" || e.kind == "disagreement")
            ++s.nullsIgnored;
    return s;
}
} // namespace roomeq
