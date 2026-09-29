#pragma once

// Show mode: how the room has changed since soundcheck (Phase 4, delta tracking).
// Port of prototype/roomeq/showtrack.py; see there for the reasoning.
//
// A reference (the output-to-mic response per third octave, transferBandsDb)
// is stored at the end of soundcheck. During the show the same response is
// measured from the music in 10 s blocks; per band, the last 2 minutes of
// blocks where the band was heard clearly are averaged and compared with it.
// The median change is a level change after the plugin, reported on its own;
// a band whose tonal change reaches 3 dB is flagged until it falls under 2 dB.

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace roomeq
{
struct ShowConfig
{
    double blockSeconds = 10.0;      // each tracking measurement
    double windowSeconds = 120.0;    // a change has to hold over this much music
    double thresholdDb = 3.0;        // flagged at this...
    double clearDb = 2.0;            // ...and cleared below this
    double minFraction = 0.5;        // of the window's blocks a band must be heard clearly in
    int minBands = 6;                // bands needed for the level change
    int referenceMinBands = 10;      // bands a reference must have heard clearly
};

// The re-check bands' usual names: "63" ... "1.25k" ... "8k".
const std::vector<std::string>& showBandNames();

struct ShowState
{
    std::vector<double> deltaDb;     // tonal change per band (level change removed), NaN = not enough music yet
    double levelDb = 0.0;            // level change after the plugin, NaN = not enough bands
    std::vector<int> flags;          // per band: +1 / -1 while flagged, else 0
    int levelFlag = 0;
    int blocks = 0;                  // blocks in the window
    int severity = 0;                // 0 none, 1 a change of 3 dB or more, 2 one of 6 dB or more
    std::string message;             // "Since soundcheck: +3.4 dB at 125–250 Hz, ..." (empty when nothing's flagged)
    std::vector<std::string> details;
};

class DeltaTracker
{
public:
    explicit DeltaTracker (std::vector<double> referenceBandsDb, const ShowConfig& config = {});

    const ShowState& addBlock (const std::vector<double>& bandsDb);
    const ShowState& state() const { return current; }
    const std::vector<double>& reference() const { return ref; }

private:
    ShowState evaluate();
    int flag (int current, double value) const;

    std::vector<double> ref;
    ShowConfig cfg;
    std::size_t capacity = 12;
    std::deque<std::vector<double>> window;
    std::vector<int> flags;
    int levelFlag = 0;
    ShowState current;
};

bool referenceIsUsable (const std::vector<double>& bandsDb, const ShowConfig& config = {});
} // namespace roomeq
