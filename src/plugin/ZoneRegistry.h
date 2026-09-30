#pragma once

#include "roomeq/alignment.h"

#include <juce_events/juce_events.h>

#include <mutex>
#include <optional>
#include <vector>

// What each instance of the plugin tells the others: its zone, its delay and
// polarity, its measured system latency and its measurements' arrivals. One
// registry is shared by every instance in the same process (the same DAW), so
// a fill or delay instance can line up with the mains without anything being
// typed. Hosts that run each plugin in its own process keep instances apart:
// there the main system's arrival is typed in instead.
//
// Message thread only (the lock keeps readers on other threads safe).
class ZoneRegistry final : public juce::ChangeBroadcaster
{
public:
    struct Measurement
    {
        int id = 0;                         // the capture's id in its instance
        juce::String name;
        roomeq::Arrival arrival;
    };

    struct Zone
    {
        juce::Uuid instance;
        juce::String label;                 // "Mains", or "Mains (PA L/R)" with the host's track name
        int zone = 0;                       // AdaptiveRoomEQProcessor::Zone
        double delayMs = 0.0;               // its zone delay
        bool invert = false;
        std::optional<double> latencyMs;    // its measured system latency
        std::vector<Measurement> measurements;   // newest last; verify captures left out

        bool operator== (const Zone& o) const;
        bool operator!= (const Zone& o) const { return ! (*this == o); }
    };

    // Replaces what `z.instance` published before (and tells listeners, if it changed).
    void publish (const Zone& z);
    void withdraw (const juce::Uuid& instance);
    std::vector<Zone> others (const juce::Uuid& self) const;

private:
    mutable std::mutex lock;
    std::vector<Zone> zones;
};

// This zone's delay to arrive with the main system's, from both arrivals (loop
// delays, both measured without their zone delays). When both instances have
// measured their system latency it's taken off each; otherwise they're assumed
// to share it (the same interface), and it cancels.
roomeq::DelaySuggestion suggestDelay (double mainArrivalMs, std::optional<double> mainLatencyMs, double mainZoneDelayMs,
                                      double zoneArrivalMs, std::optional<double> zoneLatencyMs);
