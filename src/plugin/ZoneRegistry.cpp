#include "plugin/ZoneRegistry.h"

#include <algorithm>
#include <functional>

namespace
{
bool same (double a, double b) { return std::equal_to<double>() (a, b); }
} // namespace

bool ZoneRegistry::Zone::operator== (const Zone& o) const
{
    if (instance != o.instance || label != o.label || zone != o.zone || ! same (delayMs, o.delayMs) || invert != o.invert
        || latencyMs.has_value() != o.latencyMs.has_value() || (latencyMs && ! same (*latencyMs, *o.latencyMs))
        || measurements.size() != o.measurements.size())
        return false;
    for (std::size_t i = 0; i < measurements.size(); ++i)
    {
        const auto& a = measurements[i];
        const auto& b = o.measurements[i];
        if (a.id != b.id || a.name != b.name || ! same (a.arrival.ms, b.arrival.ms) || a.arrival.confidence != b.arrival.confidence
            || a.capture != b.capture)
            return false;
    }
    return true;
}

void ZoneRegistry::publish (const Zone& z)
{
    {
        const std::lock_guard<std::mutex> guard (lock);
        const auto it = std::find_if (zones.begin(), zones.end(), [&] (const Zone& x) { return x.instance == z.instance; });
        if (it != zones.end())
        {
            if (*it == z)
                return;
            *it = z;
        }
        else
        {
            zones.push_back (z);
        }
    }
    sendChangeMessage();
}

void ZoneRegistry::withdraw (const juce::Uuid& instance)
{
    {
        const std::lock_guard<std::mutex> guard (lock);
        zones.erase (std::remove_if (zones.begin(), zones.end(), [&] (const Zone& x) { return x.instance == instance; }), zones.end());
    }
    sendChangeMessage();
}

std::vector<ZoneRegistry::Zone> ZoneRegistry::others (const juce::Uuid& self) const
{
    const std::lock_guard<std::mutex> guard (lock);
    std::vector<Zone> out;
    for (const auto& z : zones)
        if (z.instance != self)
            out.push_back (z);
    return out;
}

roomeq::DelaySuggestion suggestDelay (double mainArrivalMs, std::optional<double> mainLatencyMs, double mainZoneDelayMs,
                                      double zoneArrivalMs, std::optional<double> zoneLatencyMs)
{
    if (mainLatencyMs && zoneLatencyMs)
        return roomeq::suggestZoneDelay (mainArrivalMs - *mainLatencyMs, zoneArrivalMs - *zoneLatencyMs, mainZoneDelayMs);
    return roomeq::suggestZoneDelay (mainArrivalMs, zoneArrivalMs, mainZoneDelayMs);
}

roomeq::SubAlignment suggestSubAlignment (const roomeq::Capture& mains, std::optional<double> mainsLatencyMs, double mainsDelayMs,
                                          bool mainsInvert, const roomeq::Capture& sub, std::optional<double> subLatencyMs,
                                          double subDelayMs, bool subInvert)
{
    auto m = roomeq::lowResponse (mains), s = roomeq::lowResponse (sub);
    if (mainsLatencyMs && subLatencyMs)
    {
        m.refMs = -*mainsLatencyMs;   // flight times: each loop's latency taken off
        s.refMs = -*subLatencyMs;
    }
    roomeq::SubSettings settings;
    settings.mainDelayMs = mainsDelayMs;
    settings.mainInvert = mainsInvert;
    settings.subDelayMs = subDelayMs;
    settings.subInvert = subInvert;
    if (const auto region = roomeq::crossoverRegion (m, s))
    {
        settings.mainSnrDb = roomeq::bandSnr (mains.grade.bands, region->first, region->second);
        settings.subSnrDb = roomeq::bandSnr (sub.grade.bands, region->first, region->second);
    }
    return roomeq::alignSub (m, s, settings);
}
