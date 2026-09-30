#pragma once

// Alignment: when each speaker's sound arrives, and what delay lines them up.
// Port of prototype/roomeq/alignment.py; see there for the reasoning.
//
// An arrival is a loop delay: it includes the interface and host latency as
// well as the flight time, so arrivals are compared with each other, or have
// a measured system latency (a loopback) taken off.

#include "roomeq/capture.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>
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

// Subs: a sub's impulse response has a broad peak, so its arrival says little.
// Each sweep keeps its complex response below 1 kHz instead (Capture::low), and
// two from the same spot predict how the mains and the sub sum for any sub
// delay and polarity. See the prototype for the rules.
constexpr double subRegionDb = 10.0;   // the crossover region: within this of each other around the crossing
constexpr double subSearchMs = 20.0;   // sub delays searched: -20..+20 ms (negative: delay the mains instead)
constexpr double subNearDb = 0.25;     // choices this close sum equally well (then: not negative, normal polarity, smallest)

struct LowResponse
{
    std::vector<cplx> h;                   // on lfGrid()
    double refMs = 0.0;                    // h's time 0, after the loop start: as heard, h * exp(-j 2 pi f refMs / 1000)
};

// A sweep's (empty for pink noise and music).
LowResponse lowResponse (const Capture& c);

// The lowest SNR among graded octave bands overlapping lo-hi (infinity if none).
double bandSnr (const std::vector<BandResult>& bands, double lo, double hi);

// The crossover region's edges (Hz), where each measurement's SNR counts
// (bandSnr); none when they don't cross.
std::optional<std::pair<double, double>> crossoverRegion (const LowResponse& main, const LowResponse& sub);

struct SubSettings
{
    double mainDelayMs = 0.0;              // the mains' zone delay and polarity now
    bool mainInvert = false;
    double subDelayMs = 0.0;               // the sub's (the improvement is against them)
    bool subInvert = false;
    double mainSnrDb = std::numeric_limits<double>::infinity();   // each measurement's bandSnr over the crossover
    double subSnrDb = std::numeric_limits<double>::infinity();
};

struct SubAlignment
{
    bool ok = false;
    double regionLoHz = 0.0, regionHiHz = 0.0, crossingHz = 0.0;
    double delayMs = 0.0;                  // the sub's zone delay; negative: delay the mains by -delayMs instead
    bool invert = false;                   // the sub's polarity
    double summedDb = 0.0;                 // mean summed level over the region with the suggestion
    double improvementDb = 0.0;            // ... against the current settings
    double efficiencyDb = 0.0;             // how close to a perfect sum (0 dB)
    Confidence confidence = Confidence::high;
    std::vector<std::string> reasons;
    std::string note;
};

// The sub delay and polarity that sum best with the mains over the crossover,
// from the two measurements taken at the same spot (without their zone delays).
SubAlignment alignSub (const LowResponse& main, const LowResponse& sub, const SubSettings& settings = {});
} // namespace roomeq
