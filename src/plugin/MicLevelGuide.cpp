#include "plugin/MicLevelGuide.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
// The levels in order of loudness, and the boundaries between neighbours.
constexpr std::array<MicLevelGuide::Verdict, 6> levels { MicLevelGuide::Verdict::noSignal, MicLevelGuide::Verdict::tooQuiet,
                                                         MicLevelGuide::Verdict::good,     MicLevelGuide::Verdict::hot,
                                                         MicLevelGuide::Verdict::nearClipping, MicLevelGuide::Verdict::clipping };
constexpr std::array<float, 5> boundaries { MicLevelGuide::noSignalDb, MicLevelGuide::targetLowDb, MicLevelGuide::targetHighDb,
                                            MicLevelGuide::nearClipDb, MicLevelGuide::clipDb };
constexpr std::size_t nearClipping = 4;

std::size_t levelFor (float db)
{
    return static_cast<std::size_t> (std::count_if (boundaries.begin(), boundaries.end(), [db] (float b) { return db >= b; }));
}

std::size_t indexOf (MicLevelGuide::Verdict v)
{
    return static_cast<std::size_t> (std::find (levels.begin(), levels.end(), v) - levels.begin());   // levels.size() if not a level
}

float toDb (float gain)
{
    return gain > 1e-5f ? 20.0f * std::log10 (gain) : -100.0f;
}
} // namespace

const MicLevelGuide::Advice& MicLevelGuide::update (float micPeak, float outputPeak, double seconds, bool micConnected)
{
    seconds = std::isfinite (seconds) ? std::clamp (seconds, 0.0, 10.0) : 0.0;
    window.push_back ({ seconds, std::isfinite (micPeak) ? micPeak : 0.0f, std::isfinite (outputPeak) ? outputPeak : 0.0f });
    windowTotal += seconds;
    while (window.size() > 1 && windowTotal - window.front().seconds >= windowSeconds)
    {
        windowTotal -= window.front().seconds;
        window.pop_front();
    }

    float mic = 0.0f, output = 0.0f;
    for (const auto& t : window)
    {
        mic = std::max (mic, t.mic);
        output = std::max (output, t.output);
    }
    const auto micDb = toDb (mic);
    const auto playing = toDb (output) >= playingDb;

    const auto previous = indexOf (advice.verdict);
    auto level = levelFor (micDb);
    if (! micConnected)
    {
        level = levels.size();
        advice.verdict = Verdict::noMic;
    }
    else if (! playing && level < nearClipping)
    {
        level = levels.size();
        advice.verdict = Verdict::waiting;
    }
    else
    {
        // Only a step towards clipping shows at once; other steps need the
        // level a little past the boundary.
        if (previous < levels.size() && level != previous && level < nearClipping)
        {
            if (level > previous && micDb < boundaries[level - 1] + hysteresisDb)
                --level;
            else if (level < previous && micDb >= boundaries[level] - hysteresisDb)
                ++level;
        }
        advice.verdict = levels[level];
    }

    advice.micPeakDb = micDb;
    advice.changeDb = 0;
    if (advice.verdict == Verdict::tooQuiet)
        advice.changeDb = std::max (1, static_cast<int> (std::lround (targetLowDb + marginDb - micDb)));
    else if (advice.verdict == Verdict::hot || advice.verdict == Verdict::nearClipping || advice.verdict == Verdict::clipping)
        advice.changeDb = std::min (-1, static_cast<int> (std::lround (targetHighDb - marginDb - micDb)));
    return advice;
}

std::string MicLevelGuide::text (const Advice& a, bool standalone)
{
    const auto amount = std::to_string (std::abs (a.changeDb));
    switch (a.verdict)
    {
        case Verdict::waiting:
            return standalone ? "Start a test to check the mic level" : "Play music or noise to check the mic level";
        case Verdict::noMic: return {};
        case Verdict::noSignal: return "\xE2\x9C\x97 No mic signal: check gain, phantom, routing";
        case Verdict::tooQuiet: return "! Too quiet: mic gain up about " + amount + " dB";
        case Verdict::good: return "\xE2\x9C\x93 Mic level in the target range";
        case Verdict::hot: return "! Hot: mic gain down about " + amount + " dB";
        case Verdict::nearClipping: return "! Near clipping: mic gain down about " + amount + " dB";
        case Verdict::clipping: return "\xE2\x9C\x97 Clipping: mic gain down " + amount + "+ dB";
    }
    return {};
}

int MicLevelGuide::severity (Verdict v)
{
    switch (v)
    {
        case Verdict::good: return 0;
        case Verdict::tooQuiet:
        case Verdict::hot: return 1;
        case Verdict::noSignal:
        case Verdict::nearClipping:
        case Verdict::clipping: return 2;
        case Verdict::waiting:
        case Verdict::noMic: return -1;
    }
    return -1;
}
