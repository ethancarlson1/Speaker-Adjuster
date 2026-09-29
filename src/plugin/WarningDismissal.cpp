#include "plugin/WarningDismissal.h"

void WarningDismissal::dismiss (const roomeq::ShowState& state)
{
    if (state.severity <= 0)
    {
        reset();
        return;
    }
    flags = state.flags;
    levelFlag = state.levelFlag;
    severity = state.severity;
    active = true;
}

void WarningDismissal::update (const roomeq::ShowState& state)
{
    if (! active)
        return;
    if (state.severity <= 0 || ! hides (state))
    {
        reset();   // all clear, or something new to show
        return;
    }
    for (std::size_t b = 0; b < flags.size(); ++b)
        if (b >= state.flags.size() || state.flags[b] == 0)
            flags[b] = 0;
    if (state.levelFlag == 0)
        levelFlag = 0;
}

bool WarningDismissal::hides (const roomeq::ShowState& state) const
{
    if (! active || state.severity <= 0 || state.severity > severity)
        return false;
    for (std::size_t b = 0; b < state.flags.size(); ++b)
    {
        const auto was = b < flags.size() ? flags[b] : 0;
        if (state.flags[b] != 0 && state.flags[b] != was)
            return false;
    }
    return state.levelFlag == 0 || state.levelFlag == levelFlag;
}

void WarningDismissal::reset()
{
    flags.clear();
    levelFlag = severity = 0;
    active = false;
}
