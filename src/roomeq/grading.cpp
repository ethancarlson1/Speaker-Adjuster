#include "roomeq/grading.h"

#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace roomeq
{
namespace
{
Grade gradeValue (double value, double passAt, double marginalAt, bool higherIsBetter)
{
    if (higherIsBetter)
        return value >= passAt ? Grade::pass : value >= marginalAt ? Grade::marginal : Grade::redo;
    return value <= passAt ? Grade::pass : value <= marginalAt ? Grade::marginal : Grade::redo;
}

std::string format (const char* fmt, double value)
{
    char buf[64];
    std::snprintf (buf, sizeof (buf), fmt, value);
    return buf;
}

// Repeat-to-repeat level spread per 1/3 octave. Steady noise alone makes
// repeats wobble (each repeat's band level has std ~ 4.34 sqrt(2 / (n s)) dB),
// so only spread beyond 3 sigma of that (for a pair) is attributed to an event.
// Returns (raw, excess) for the third with the largest excess.
std::pair<double, double> worstThirdOctaveSpread (const std::vector<double>& freqs,
                                                  const std::vector<std::vector<double>>& repeats,
                                                  const std::vector<double>& signal,
                                                  const std::vector<double>& noise, double lo, double hi)
{
    const auto k = static_cast<double> (repeats.size());
    double worstRaw = 0.0;
    double worstExcess = -std::numeric_limits<double>::infinity();
    for (int j = 0; j < 3; ++j)
    {
        const auto a = lo * std::pow (2.0, j / 3.0);
        const auto b = std::min (lo * std::pow (2.0, (j + 1) / 3.0), hi);
        if (b <= a)
            break;
        double maxLevel = -std::numeric_limits<double>::infinity();
        double minLevel = std::numeric_limits<double>::infinity();
        for (const auto& p : repeats)
        {
            const auto level = 10.0 * std::log10 (std::max (bandSum (freqs, p, a, b).sum, 1e-30));
            maxLevel = std::max (maxLevel, level);
            minLevel = std::min (minLevel, level);
        }
        const auto raw = maxLevel - minLevel;
        const auto s = bandSum (freqs, signal, a, b);
        const auto nz = bandSum (freqs, noise, a, b).sum;
        const auto snrSingle = std::max (s.sum, 1e-30) / std::max (nz * k, 1e-30);
        const auto sigma = 4.34 * std::sqrt (2.0 / (std::max (s.count, 1) * snrSingle));
        const auto excess = raw - 3.0 * std::sqrt (2.0) * sigma;
        if (excess > worstExcess)
        {
            worstRaw = raw;
            worstExcess = excess;
        }
    }
    return { worstRaw, std::max (0.0, worstExcess) };
}

// Contiguous runs (by index) of bands whose key(band) is a non-pass grade.
std::vector<std::vector<std::size_t>> runs (const std::vector<BandResult>& bands,
                                            const std::function<Grade (const BandResult&)>& key)
{
    std::vector<std::vector<std::size_t>> out;
    std::vector<std::size_t> current;
    for (std::size_t i = 0; i < bands.size(); ++i)
    {
        if (! bands[i].outOfRange && key (bands[i]) > Grade::pass)
        {
            current.push_back (i);
        }
        else if (! current.empty())
        {
            out.push_back (current);
            current.clear();
        }
    }
    if (! current.empty())
        out.push_back (current);
    return out;
}

std::string span (const std::vector<BandResult>& bands, const std::vector<std::size_t>& run,
                  std::size_t firstGraded, std::size_t lastGraded)
{
    const auto lo = formatHz (bands[run.front()].lo);
    const auto hi = formatHz (bands[run.back()].hi);
    const auto atBottom = run.front() == firstGraded;
    const auto atTop = run.back() == lastGraded;
    if (atBottom && ! atTop)
        return "below " + hi;
    if (atTop && ! atBottom)
        return "above " + lo;
    return lo + "\xE2\x80\x93" + hi;   // en dash
}

std::vector<std::string> makeReasons (const std::vector<BandResult>& bands)
{
    std::vector<std::size_t> graded;
    for (std::size_t i = 0; i < bands.size(); ++i)
        if (! bands[i].outOfRange)
            graded.push_back (i);
    if (graded.empty())
        return {};

    std::vector<std::string> reasons;
    for (const auto& run : runs (bands, [] (const BandResult& b) { return b.snrGrade; }))
    {
        double worst = std::numeric_limits<double>::infinity();
        auto severity = Grade::pass;
        for (auto i : run)
        {
            worst = std::min (worst, bands[i].snrDb);
            severity = std::max (severity, bands[i].snrGrade);
        }
        const auto where = span (bands, run, graded.front(), graded.back());
        const auto prefix = where.rfind ("below", 0) == 0 ? "low-end " : where.rfind ("above", 0) == 0 ? "high-frequency " : "";
        reasons.push_back (std::string (gradeLabel (severity)) + ": " + prefix + "noise too high " + where
                           + " (SNR " + format ("%.0f", worst) + " dB)");
    }

    // Low SNR makes repeats disagree too; only report inconsistency that
    // steady noise doesn't already explain.
    const auto consistencyKey = [] (const BandResult& b)
    { return b.consistencyGrade > b.snrGrade ? b.consistencyGrade : Grade::pass; };
    for (const auto& run : runs (bands, consistencyKey))
    {
        double worst = 0.0;
        auto severity = Grade::pass;
        for (auto i : run)
        {
            worst = std::max (worst, bands[i].spreadDb.value_or (0.0));
            severity = std::max (severity, bands[i].consistencyGrade);
        }
        reasons.push_back (std::string (gradeLabel (severity)) + ": repeat sweeps differ by " + format ("%.1f", worst)
                           + " dB " + span (bands, run, graded.front(), graded.back())
                           + " (noise burst or movement during a sweep?)");
    }
    return reasons;
}

std::vector<std::string> rangeNotes (const std::vector<BandResult>& bands)
{
    // The out-of-range runs at each end (not halves of the spectrum: a sub's
    // range sits at the bottom, so its high run starts in the low half).
    std::size_t nLow = 0, nHigh = 0;
    while (nLow < bands.size() && bands[nLow].outOfRange)
        ++nLow;
    while (nHigh < bands.size() - nLow && bands[bands.size() - 1 - nHigh].outOfRange)
        ++nHigh;
    std::vector<std::string> notes;
    if (nLow > 0)
        notes.push_back ("below the PA's range under " + formatHz (bands[nLow - 1].hi) + " (not graded)");
    if (nHigh > 0)
        notes.push_back ("above the PA's range over " + formatHz (bands[bands.size() - nHigh].lo) + " (not graded)");
    return notes;
}

CaptureGrade verdict (std::vector<BandResult> bands, const GradingConfig& cfg)
{
    CaptureGrade result;
    result.bands = std::move (bands);

    // Out of range = contiguous run of low-level bands at either end of the spectrum.
    for (std::size_t i = 0; i < result.bands.size() && result.bands[i].levelDb < -cfg.outOfRangeDb; ++i)
        result.bands[i].outOfRange = true;
    for (std::size_t i = result.bands.size(); i-- > 0 && result.bands[i].levelDb < -cfg.outOfRangeDb;)
        result.bands[i].outOfRange = true;

    bool anyGraded = false;
    bool anySpread = false;
    auto overall = Grade::pass;
    for (const auto& b : result.bands)
    {
        if (const auto g = b.grade())
        {
            anyGraded = true;
            overall = std::max (overall, *g);
            anySpread = anySpread || b.spreadDb.has_value();
        }
    }
    result.overall = anyGraded ? overall : Grade::redo;
    result.reasons = makeReasons (result.bands);
    result.notes = rangeNotes (result.bands);

    if (! anyGraded)
    {
        result.reasons = { "no usable signal: check the mic is connected and the PA is playing" };
    }
    else if (result.overall == Grade::pass)
    {
        auto text = "all bands \xE2\x89\xA5 " + format ("%.0f", cfg.snrPassDb) + " dB SNR";   // ≥
        if (anySpread)
            text += ", repeats within " + format ("%g", cfg.consistencyPassDb) + " dB";
        result.reasons = { text };
    }
    return result;
}
} // namespace

const char* gradeLabel (Grade g)
{
    switch (g)
    {
        case Grade::pass: return "pass";
        case Grade::marginal: return "marginal";
        case Grade::redo: return "redo";
    }
    return "";
}

std::optional<Grade> BandResult::grade() const
{
    if (outOfRange)
        return std::nullopt;
    return std::max (snrGrade, consistencyGrade);
}

CaptureGrade gradeCapture (const std::vector<double>& freqs, const std::vector<double>& signalPower,
                           const std::vector<double>& noisePower,
                           const std::vector<std::vector<double>>* repeatPowers,
                           const GradingConfig& cfg, double fMax)
{
    const auto pb = bandSum (freqs, signalPower, cfg.passbandLo, cfg.passbandHi);
    const auto passbandDensity = std::max (pb.sum, 1e-30) / std::max (pb.count, 1);
    const auto haveRepeats = repeatPowers != nullptr && repeatPowers->size() > 1;

    std::vector<BandResult> bands;
    for (std::size_t j = 0; j < octaveCenters.size(); ++j)
    {
        BandResult b;
        b.name = octaveNominal[j];
        b.center = octaveCenters[j];
        const auto edges = octaveEdges (b.center);
        b.lo = edges.first;
        b.hi = std::min (edges.second, fMax);

        const auto s = bandSum (freqs, signalPower, b.lo, b.hi);
        const auto nz = bandSum (freqs, noisePower, b.lo, b.hi).sum;
        const auto sig = std::max (s.sum, 1e-30);
        b.levelDb = 10.0 * std::log10 ((sig / std::max (s.count, 1)) / passbandDensity);
        b.snrDb = 10.0 * std::log10 (sig / std::max (nz, 1e-30));
        b.snrGrade = gradeValue (b.snrDb, cfg.snrPassDb, cfg.snrMarginalDb, true);

        if (haveRepeats)
        {
            const auto [raw, excess] = worstThirdOctaveSpread (freqs, *repeatPowers, signalPower, noisePower, b.lo, b.hi);
            b.spreadDb = raw;
            b.excessSpreadDb = excess;
            b.consistencyGrade = gradeValue (excess, cfg.consistencyPassDb, cfg.consistencyMarginalDb, false);
        }
        bands.push_back (b);
    }
    return verdict (std::move (bands), cfg);
}

CaptureGrade regrade (const CaptureGrade& grade, const std::vector<double>& freqs, const std::vector<double>& signalPower,
                      const GradingConfig& cfg)
{
    const auto pb = bandSum (freqs, signalPower, cfg.passbandLo, cfg.passbandHi);
    const auto passbandDensity = std::max (pb.sum, 1e-30) / std::max (pb.count, 1);
    std::vector<BandResult> bands;
    for (auto b : grade.bands)
    {
        const auto s = bandSum (freqs, signalPower, b.lo, b.hi);
        b.levelDb = 10.0 * std::log10 ((std::max (s.sum, 1e-30) / std::max (s.count, 1)) / passbandDensity);
        b.outOfRange = false;
        b.snrGrade = gradeValue (b.snrDb, cfg.snrPassDb, cfg.snrMarginalDb, true);
        b.consistencyGrade = b.excessSpreadDb ? gradeValue (*b.excessSpreadDb, cfg.consistencyPassDb, cfg.consistencyMarginalDb, false)
                                              : Grade::pass;
        bands.push_back (b);
    }
    auto result = verdict (std::move (bands), cfg);
    // Notes that didn't come from grading (clock drift) follow the range notes.
    const auto rangeCount = std::min (rangeNotes (grade.bands).size(), grade.notes.size());
    result.notes.insert (result.notes.end(), grade.notes.begin() + static_cast<std::ptrdiff_t> (rangeCount), grade.notes.end());
    return result;
}
} // namespace roomeq
