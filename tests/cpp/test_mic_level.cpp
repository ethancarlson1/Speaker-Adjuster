#include "plugin/MicLevelGuide.h"

#include <doctest/doctest.h>

#include <cmath>

namespace
{
using Verdict = MicLevelGuide::Verdict;

float gain (double db) { return static_cast<float> (std::pow (10.0, db / 20.0)); }

constexpr float playing = 0.25f;   // the plugin's output peaks (-12 dBFS): something is playing

// A fresh reading: one tick longer than the window replaces everything before it.
const MicLevelGuide::Advice& settle (MicLevelGuide& g, double micDb, float output = playing)
{
    return g.update (gain (micDb), output, MicLevelGuide::windowSeconds + 1.0, true);
}
} // namespace

TEST_CASE ("mic level guide: verdicts and advice while something plays")
{
    MicLevelGuide g;
    auto a = settle (g, -45.0);
    CHECK (a.verdict == Verdict::tooQuiet);
    CHECK (a.changeDb == 20);   // aims 5 dB inside the zone: -25 dBFS
    CHECK (MicLevelGuide::text (a, false) == "! Too quiet: mic gain up about 20 dB");

    a = settle (g, -20.0);
    CHECK (a.verdict == Verdict::good);
    CHECK (a.changeDb == 0);
    CHECK (MicLevelGuide::text (a, false) == "\xE2\x9C\x93 Mic level in the target range");

    a = settle (g, -6.0);
    CHECK (a.verdict == Verdict::hot);
    CHECK (a.changeDb == -9);   // aims at -15 dBFS
    CHECK (MicLevelGuide::text (a, false) == "! Hot: mic gain down about 9 dB");

    a = settle (g, -2.0);
    CHECK (a.verdict == Verdict::nearClipping);
    CHECK (a.changeDb == -13);

    a = settle (g, 0.0);
    CHECK (a.verdict == Verdict::clipping);
    CHECK (MicLevelGuide::text (a, false) == "\xE2\x9C\x97 Clipping: mic gain down 15+ dB");

    MicLevelGuide dead;
    a = settle (dead, -95.0);
    CHECK (a.verdict == Verdict::noSignal);
    CHECK (a.changeDb == 0);
    CHECK (MicLevelGuide::severity (a.verdict) == 2);
}

TEST_CASE ("mic level guide: nothing playing, no mic")
{
    MicLevelGuide g;
    auto a = settle (g, -70.0, 0.0f);   // the room's noise, nothing through the PA
    CHECK (a.verdict == Verdict::waiting);
    CHECK (MicLevelGuide::text (a, false) == "Play music or noise to check the mic level");
    CHECK (MicLevelGuide::text (a, true) == "Start a test to check the mic level");
    CHECK (MicLevelGuide::severity (a.verdict) == -1);

    a = settle (g, -20.0, gain (-50.0));   // a very quiet output still doesn't count
    CHECK (a.verdict == Verdict::waiting);

    a = settle (g, -1.0, 0.0f);            // clipping is worth saying whatever plays
    CHECK (a.verdict == Verdict::nearClipping);
    a = settle (g, 0.5, 0.0f);
    CHECK (a.verdict == Verdict::clipping);

    a = g.update (gain (-20.0), playing, 4.0, false);
    CHECK (a.verdict == Verdict::noMic);
    CHECK (MicLevelGuide::text (a, false).empty());
}

TEST_CASE ("mic level guide: judged on the loudest peak of the last few seconds")
{
    MicLevelGuide g;
    settle (g, -45.0);
    g.update (gain (-20.0), playing, 0.1, true);    // one loud moment
    for (int i = 0; i < 20; ++i)                    // then 2 s of quieter music
        g.update (gain (-45.0), playing, 0.1, true);
    CHECK (g.get().verdict == Verdict::good);
    CHECK (g.get().micPeakDb == doctest::Approx (-20.0).epsilon (0.001));
    for (int i = 0; i < 15; ++i)                    // 3.5 s after it: gone
        g.update (gain (-45.0), playing, 0.1, true);
    CHECK (g.get().verdict == Verdict::tooQuiet);

    // Output that stopped more than the window ago: back to waiting.
    for (int i = 0; i < 40; ++i)
        g.update (gain (-45.0), 0.0f, 0.1, true);
    CHECK (g.get().verdict == Verdict::waiting);
}

TEST_CASE ("mic level guide: advice doesn't flicker at the zone's edges")
{
    MicLevelGuide g;
    CHECK (settle (g, -20.0).verdict == Verdict::good);
    CHECK (settle (g, -30.5).verdict == Verdict::good);       // within 1 dB of the edge: stays
    CHECK (settle (g, -31.5).verdict == Verdict::tooQuiet);
    CHECK (settle (g, -29.5).verdict == Verdict::tooQuiet);   // and back: stays until clearly in
    CHECK (settle (g, -28.5).verdict == Verdict::good);
    CHECK (settle (g, -9.5).verdict == Verdict::good);
    CHECK (settle (g, -8.5).verdict == Verdict::hot);
    CHECK (settle (g, -10.5).verdict == Verdict::hot);
    CHECK (settle (g, -11.5).verdict == Verdict::good);
    CHECK (settle (g, -2.9).verdict == Verdict::nearClipping);   // towards clipping: at once
    CHECK (settle (g, -3.5).verdict == Verdict::nearClipping);
    CHECK (settle (g, -4.5).verdict == Verdict::hot);

    // From waiting there's no previous level to hold on to.
    MicLevelGuide h;
    settle (h, -20.0, 0.0f);
    CHECK (settle (h, -29.5).verdict == Verdict::good);
    MicLevelGuide k;
    settle (k, -20.0, 0.0f);
    CHECK (settle (k, -30.5).verdict == Verdict::tooQuiet);
}

TEST_CASE ("mic level guide: bad input")
{
    MicLevelGuide g;
    auto a = g.update (std::nanf (""), playing, 4.0, true);
    CHECK (a.verdict == Verdict::noSignal);
    a = g.update (gain (-20.0), playing, std::nan (""), true);   // no time passed: still the same reading
    CHECK (a.verdict == Verdict::good);
    a = g.update (gain (-20.0), playing, -1.0, true);
    CHECK (a.verdict == Verdict::good);
}
