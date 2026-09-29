#pragma once

// The show warning's × (on the banner): hides the warning it was given until
// something new comes up, i.e. a band (or the overall level) flagged that
// wasn't, or the change growing past the severity that was dismissed (3 dB to
// 6 dB). A band that clears drops out of what was dismissed, so if it trips
// again that's new. Once nothing is flagged, the dismissal is over.
// JUCE-free, so it's unit tested.

#include "roomeq/showtrack.h"

#include <vector>

class WarningDismissal
{
public:
    void dismiss (const roomeq::ShowState& state);
    void update (const roomeq::ShowState& state);          // with each new state
    bool hides (const roomeq::ShowState& state) const;
    bool isActive() const { return active; }
    void reset();

private:
    std::vector<int> flags;
    int levelFlag = 0;
    int severity = 0;
    bool active = false;
};
