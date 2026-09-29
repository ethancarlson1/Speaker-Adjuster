#pragma once

// Mic gain advice for the header's Mic meter: the loudest mic peak of the last
// few seconds against a target zone. It only judges while something plays
// through the PA (the plugin's own output), so a quiet room never reads as
// "turn it up"; clipping is reported whatever is playing. Levels move a little
// past a boundary before the advice changes, so it doesn't flicker.
// JUCE-free, so it's unit tested.

#include <deque>
#include <string>

class MicLevelGuide
{
public:
    static constexpr float targetLowDb = -30.0f;   // the zone for the mic's peaks, dBFS
    static constexpr float targetHighDb = -10.0f;
    static constexpr float nearClipDb = -3.0f;
    static constexpr float clipDb = -0.1f;
    static constexpr float noSignalDb = -80.0f;    // peaks below this while playing: nothing from the mic
    static constexpr float playingDb = -40.0f;     // output peaks above this: something is playing
    static constexpr float marginDb = 5.0f;        // advice aims this far inside the zone
    static constexpr float hysteresisDb = 1.0f;
    static constexpr double windowSeconds = 3.0;

    enum class Verdict
    {
        waiting,        // nothing playing, so there's nothing to judge
        noMic,          // no mic input connected (the header says so already)
        noSignal,
        tooQuiet,
        good,
        hot,
        nearClipping,
        clipping
    };

    struct Advice
    {
        Verdict verdict = Verdict::waiting;
        float micPeakDb = -100.0f;   // loudest in the window
        int changeDb = 0;            // suggested gain change: + up, - down, 0 for none
    };

    // At the meter rate: the linear peaks since the last call, and how long that was.
    const Advice& update (float micPeak, float outputPeak, double seconds, bool micConnected);
    const Advice& get() const { return advice; }

    // One short line for under the meter ("" when there's nothing to add), UTF-8.
    static std::string text (const Advice& a, bool standalone);
    // 0 good, 1 worth a look, 2 fix it before measuring, -1 nothing to judge.
    static int severity (Verdict v);

private:
    struct Tick
    {
        double seconds;
        float mic, output;
    };
    std::deque<Tick> window;
    double windowTotal = 0.0;
    Advice advice;
};
