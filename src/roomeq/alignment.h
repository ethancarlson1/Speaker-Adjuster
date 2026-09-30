#pragma once

// Alignment: when each speaker's sound arrives, and what delay lines them up.
// Port of prototype/roomeq/alignment.py; see there for the reasoning.
//
// An arrival is a loop delay: it includes the interface and host latency as
// well as the flight time, so arrivals are compared with each other, or have
// a measured system latency (a loopback) taken off.

#include "roomeq/capture.h"

#include <string>
#include <vector>

namespace roomeq
{
enum class Confidence
{
    high = 0,
    medium,
    low
};
const char* confidenceLabel (Confidence c);   // "high", "medium", "low"

constexpr double speedOfSound = 343.0;        // m/s, about 20 °C
constexpr double firstArrivalDb = 6.0;        // the direct sound: the first peak within this of the strongest
constexpr double earlierWarningDb = 12.0;     // an earlier peak within this of the strongest is worth a mention

struct Arrival
{
    double ms = 0.0;                          // loop delay of the direct arrival
    Confidence confidence = Confidence::high;
    std::vector<std::string> reasons;
};

// The capture's direct arrival (sweeps: the first peak of the impulse response
// within 6 dB of the strongest, refined below a sample; pink noise and music:
// the dual-FFT's delay) and how far to trust it. separationMs: how far before
// the arrival an earlier peak has to be to count as a separate arrival.
Arrival estimateArrival (const Capture& c, double separationMs = 1.0, const WindowConfig& window = {});

// How far sound travels in the arrival time, less a measured system latency.
// An equivalent acoustic distance, not necessarily the physical one.
double equivalentDistanceM (double arrivalMs, double latencyMs = 0.0);

struct DelaySuggestion
{
    double delayMs = 0.0;          // add this to the zone's delay (never negative)
    double differenceMs = 0.0;     // main arrival - this zone's arrival (both measured without their zone delays)
    std::string note;
};

// The delay that makes this zone's sound arrive with the main system's, at the
// spot both were measured at. Measurements bypass the zone delays, so the main
// system's own zone delay (if any) is added back.
DelaySuggestion suggestZoneDelay (double mainMs, double zoneMs, double mainZoneDelayMs = 0.0);
} // namespace roomeq
