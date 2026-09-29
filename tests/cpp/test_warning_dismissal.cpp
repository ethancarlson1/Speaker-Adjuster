#include "plugin/WarningDismissal.h"

#include <doctest/doctest.h>

namespace
{
// A state with the given bands flagged (+1 up, -1 down) out of 22.
roomeq::ShowState flagged (std::vector<std::pair<int, int>> bands, int severity, int levelFlag = 0)
{
    roomeq::ShowState s;
    s.flags.assign (22, 0);
    for (const auto& [b, f] : bands)
        s.flags[static_cast<std::size_t> (b)] = f;
    s.levelFlag = levelFlag;
    s.severity = severity;
    return s;
}
} // namespace

TEST_CASE ("warning dismissal: hidden while nothing new")
{
    WarningDismissal d;
    const auto buildUp = flagged ({ { 4, 1 }, { 5, 1 } }, 1);
    CHECK_FALSE (d.hides (buildUp));
    d.dismiss (buildUp);
    CHECK (d.hides (buildUp));
    d.update (buildUp);
    CHECK (d.hides (buildUp));

    const auto oneCleared = flagged ({ { 5, 1 } }, 1);   // fewer bands: still nothing new
    d.update (oneCleared);
    CHECK (d.hides (oneCleared));
    CHECK (d.isActive());
}

TEST_CASE ("warning dismissal: something new shows it again")
{
    const auto buildUp = flagged ({ { 4, 1 }, { 5, 1 } }, 1);
    {
        WarningDismissal d;   // another band
        d.dismiss (buildUp);
        const auto more = flagged ({ { 4, 1 }, { 5, 1 }, { 18, -1 } }, 1);
        CHECK_FALSE (d.hides (more));
        d.update (more);
        CHECK_FALSE (d.isActive());   // it's shown until dismissed again
    }
    {
        WarningDismissal d;   // the same band the other way
        d.dismiss (buildUp);
        CHECK_FALSE (d.hides (flagged ({ { 4, -1 }, { 5, 1 } }, 1)));
    }
    {
        WarningDismissal d;   // 3 dB grows to 6 dB
        d.dismiss (buildUp);
        CHECK_FALSE (d.hides (flagged ({ { 4, 1 }, { 5, 1 } }, 2)));
    }
    {
        WarningDismissal d;   // the overall level moves
        d.dismiss (buildUp);
        CHECK_FALSE (d.hides (flagged ({ { 4, 1 }, { 5, 1 } }, 1, -1)));
    }
    {
        WarningDismissal d;   // dismissed at 6 dB: back at 3 dB it stays hidden
        d.dismiss (flagged ({ { 4, 1 } }, 2));
        CHECK (d.hides (flagged ({ { 4, 1 } }, 1)));
    }
}

TEST_CASE ("warning dismissal: a band that clears and trips again is new; all clear ends it")
{
    WarningDismissal d;
    d.dismiss (flagged ({ { 4, 1 }, { 5, 1 } }, 1));
    d.update (flagged ({ { 5, 1 } }, 1));                        // band 4 cleared
    CHECK_FALSE (d.hides (flagged ({ { 4, 1 }, { 5, 1 } }, 1)));  // and tripped again

    WarningDismissal e;
    e.dismiss (flagged ({ { 4, 1 } }, 1));
    e.update (flagged ({}, 0));                                  // all clear
    CHECK_FALSE (e.isActive());
    CHECK_FALSE (e.hides (flagged ({ { 4, 1 } }, 1)));

    WarningDismissal f;
    f.dismiss (flagged ({}, 0));                                 // nothing to dismiss
    CHECK_FALSE (f.isActive());
}
