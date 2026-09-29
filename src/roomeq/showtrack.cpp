#include "roomeq/showtrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace roomeq
{
namespace
{
const std::string enDash = "\xe2\x80\x93";

bool isK (const std::string& name) { return ! name.empty() && name.back() == 'k'; }
std::string withoutK (const std::string& name) { return name.substr (0, name.size() - 1); }

std::string rangeName (std::size_t lo, std::size_t hi)
{
    const auto& names = showBandNames();
    const auto& a = names[lo];
    const auto& b = names[hi];
    if (lo == hi)
        return isK (a) ? withoutK (a) + " kHz" : a + " Hz";
    if (isK (a) && isK (b))
        return withoutK (a) + enDash + withoutK (b) + " kHz";
    if (isK (b))
        return a + " Hz" + enDash + withoutK (b) + " kHz";
    return a + enDash + b + " Hz";
}

std::string oneDecimal (double v)
{
    char text[32];
    std::snprintf (text, sizeof (text), "%.1f", v);
    return text;
}

// Signed, one decimal, rounded like printf (as the prototype's f-strings are).
std::string db (double v)
{
    const auto text = oneDecimal (std::abs (v));
    return text == "0.0" ? text : (v > 0.0 ? "+" : "-") + text;
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const auto m = v.size() / 2;
    return v.size() % 2 == 1 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}
} // namespace

const std::vector<std::string>& showBandNames()
{
    static const std::vector<std::string> names { "63",  "80",  "100", "125",  "160", "200", "250", "315",
                                                  "400", "500", "630", "800",  "1k",  "1.25k", "1.6k", "2k",
                                                  "2.5k", "3.15k", "4k", "5k", "6.3k", "8k" };
    return names;
}

DeltaTracker::DeltaTracker (std::vector<double> referenceBandsDb, const ShowConfig& config)
    : ref (std::move (referenceBandsDb)), cfg (config)
{
    capacity = static_cast<std::size_t> (std::max (1L, std::lround (cfg.windowSeconds / cfg.blockSeconds)));
    flags.assign (ref.size(), 0);
    current = evaluate();
}

const ShowState& DeltaTracker::addBlock (const std::vector<double>& bandsDb)
{
    window.push_back (bandsDb);
    if (window.size() > capacity)
        window.pop_front();
    current = evaluate();
    return current;
}

int DeltaTracker::flag (int currentFlag, double value) const
{
    if (! std::isfinite (value))
        return 0;   // not enough clear music in the window: no claim
    const auto sign = value > 0.0 ? 1 : -1;
    if (currentFlag != 0 && currentFlag == sign && std::abs (value) >= cfg.clearDb)
        return currentFlag;
    return std::abs (value) >= cfg.thresholdDb ? sign : 0;
}

ShowState DeltaTracker::evaluate()
{
    const auto n = ref.size();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto need = cfg.minFraction * static_cast<double> (capacity);

    std::vector<double> raw (n, nan);
    for (std::size_t b = 0; b < n; ++b)
    {
        double sum = 0.0;
        int count = 0;
        for (const auto& blk : window)
            if (b < blk.size() && std::isfinite (blk[b]))
            {
                sum += blk[b];
                ++count;
            }
        if (count > 0 && static_cast<double> (count) >= need && std::isfinite (ref[b]))
            raw[b] = sum / count - ref[b];
    }
    std::vector<double> finite;
    for (auto v : raw)
        if (std::isfinite (v))
            finite.push_back (v);
    const auto level = static_cast<int> (finite.size()) >= cfg.minBands ? median (finite) : nan;
    std::vector<double> delta (n, nan);
    if (std::isfinite (level))
        for (std::size_t b = 0; b < n; ++b)
            delta[b] = raw[b] - level;

    for (std::size_t b = 0; b < n; ++b)
        flags[b] = flag (flags[b], delta[b]);
    levelFlag = flag (levelFlag, level);

    ShowState s;
    s.deltaDb = delta;
    s.levelDb = level;
    s.flags = flags;
    s.levelFlag = levelFlag;
    s.blocks = static_cast<int> (window.size());

    std::vector<double> sizes;
    for (std::size_t b = 0; b < n; ++b)
        if (flags[b] != 0)
            sizes.push_back (std::abs (delta[b]));
    if (levelFlag != 0)
        sizes.push_back (std::abs (level));
    s.severity = sizes.empty() ? 0 : (*std::max_element (sizes.begin(), sizes.end()) >= 6.0 ? 2 : 1);

    // Neighbouring flagged bands that moved the same way read as one range.
    std::vector<std::string> parts;
    for (std::size_t b = 0; b < n;)
    {
        if (flags[b] == 0)
        {
            ++b;
            continue;
        }
        auto e = b;
        while (e + 1 < n && flags[e + 1] == flags[b])
            ++e;
        auto peak = delta[b];
        for (auto i = b + 1; i <= e; ++i)
            if (std::abs (delta[i]) > std::abs (peak))
                peak = delta[i];
        parts.push_back (db (peak) + " dB at " + rangeName (b, e));
        b = e + 1;
    }
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite (delta[i]))
            s.details.push_back (rangeName (i, i) + ": " + db (delta[i]) + " dB" + (flags[i] != 0 ? "  (flagged)" : ""));
    if (levelFlag != 0)
        parts.push_back ("everything " + oneDecimal (std::abs (level)) + " dB " + (level > 0.0 ? "louder" : "quieter")
                         + " after the plugin");
    if (! parts.empty())
    {
        s.message = "Since soundcheck: ";
        for (std::size_t i = 0; i < parts.size(); ++i)
            s.message += (i > 0 ? ", " : "") + parts[i];
    }
    return s;
}

bool referenceIsUsable (const std::vector<double>& bandsDb, const ShowConfig& config)
{
    const auto count = std::count_if (bandsDb.begin(), bandsDb.end(), [] (double v) { return std::isfinite (v); });
    return count >= config.referenceMinBands;
}
} // namespace roomeq
