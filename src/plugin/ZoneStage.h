#pragma once

// The zone's time alignment and polarity, last in the chain: a delay of up to
// 300 ms (to line a delay stack or a front fill up with the mains) and a
// polarity flip. JUCE-free, so it's unit tested.
//
// A change crossfades over 20 ms from the old setting to the new one, so moving
// the delay or flipping the polarity never clicks (a change that arrives
// mid-fade starts when that fade ends). Delays are band-limited fractional
// ones (a 32-tap Kaiser-windowed sinc, flat to within 0.002 dB up to 20 kHz at
// 48 kHz) from 15 samples up; below that, the nearest whole sample. At 0 ms and
// normal polarity the audio passes unchanged. It isn't reported to the host as
// latency: the delay is there on purpose.
//
// The audio thread never allocates: prepare() sizes the delay line for the
// longest delay at the sample rate.

#include <array>
#include <functional>
#include <string>
#include <vector>

struct ZoneSettings
{
    double delayMs = 0.0;
    bool invert = false;

    bool operator== (const ZoneSettings& o) const noexcept { return std::equal_to<double>() (delayMs, o.delayMs) && invert == o.invert; }
    bool operator!= (const ZoneSettings& o) const noexcept { return ! (*this == o); }
};

// "12.50 ms (4.29 m / 14.1 ft)": the delay and the distance sound travels in
// it (343 m/s, about 20 °C).
std::string zoneDelayText (double delayMs);
// Typed text back to ms: "12.5" or "12.5 ms", or a distance ("4.3 m", "430 cm",
// "14 ft") converted at that speed. A comma works as the decimal point. Clamped to 0-300 ms.
double parseZoneDelay (const std::string& text);
constexpr double speedOfSound = 343.0;

class ZoneStage
{
public:
    static constexpr double maxDelayMs = 300.0;
    static constexpr double fadeSeconds = 0.02;
    static constexpr int maxChannels = 2;
    static constexpr int taps = 32;

    // Audio stopped. Starts at `initial` (no fade in) with an empty delay line.
    void prepare (double sampleRate, const ZoneSettings& initial = {});

    // Audio thread.
    void process (float* const* channels, int numChannels, int numSamples, const ZoneSettings& target) noexcept;

    // What's playing: the fade's destination while fading.
    ZoneSettings getCurrent() const noexcept { return isFading() ? next.settings : current.settings; }
    bool isFading() const noexcept { return fadePos < fadeLength; }

    // Samples of delay a setting gives at `sampleRate` (what the stage plays).
    static double delaySamples (double delayMs, double sampleRate) noexcept;

private:
    struct Tap
    {
        ZoneSettings settings;
        int whole = 0;                    // integer delay; with a kernel, the delay of its first tap + 15
        bool fractional = false;
        std::array<float, taps> kernel {};   // includes the polarity
        float gain = 1.0f;                // whole-sample taps
    };

    Tap makeTap (const ZoneSettings& s) const noexcept;
    float read (const Tap& t, const std::vector<float>& line) const noexcept;

    double fs = 48000.0;
    std::array<std::vector<float>, maxChannels> lines;
    int mask = 0, writePos = 0;
    Tap current, next;
    int fadeLength = 1, fadePos = 1;
};
