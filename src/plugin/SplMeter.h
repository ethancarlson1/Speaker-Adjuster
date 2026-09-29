#pragma once

// The show view's SPL meter. JUCE-free, so it's unit tested.
//
// Audio thread: A- and C-weighting (IEC 61672) on the mic, the fast (125 ms)
// A-weighted level and its peak, and the mean square of each second, A and C,
// handed over through a lock-free ring. It never allocates, locks or waits.
//
// Message thread: LAeq and LCeq over the last 15 minutes, and the max fast
// level since a reset. Levels here are dBFS; the mic calibration's offset
// turns them into dB SPL. Seconds with nothing from the mic (below -100 dBFS)
// aren't counted: a dead mic isn't silence.

#include "roomeq/filters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class SplMeter
{
public:
    static constexpr double fastSeconds = 0.125;
    static constexpr int leqSeconds = 15 * 60;
    static constexpr float noneDb = -200.0f;

    // Audio stopped.
    void prepare (double sampleRate);

    // Audio thread.
    void process (const float* mic, int numSamples) noexcept;

    // Message thread.
    void collect();                                     // takes the finished seconds and the peak
    void reset();                                       // the Leqs and the max start over
    float fastDb() const noexcept { return fast.load (std::memory_order_relaxed); }   // LAF now
    float maxDb() const noexcept { return maxFast; }    // LAFmax since the reset (noneDb before any)
    double laeqDb() const noexcept { return leqDb (0); }   // over the seconds heard, up to 15 min
    double lceqDb() const noexcept { return leqDb (1); }
    int secondsHeard() const noexcept { return count; }

private:
    struct Section
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double s1 = 0.0, s2 = 0.0;
        double tick (double x) noexcept
        {
            const auto y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }
    };
    struct Second
    {
        double a = 0.0, c = 0.0;   // mean squares
    };
    double leqDb (int which) const noexcept;

    // Audio thread.
    std::array<Section, 3> weightA {};
    std::array<Section, 2> weightC {};
    double gainA = 1.0, gainC = 1.0;
    double fastCoeff = 0.0, fastMs = 0.0;
    double accA = 0.0, accC = 0.0;
    long samplesPerSecond = 48000, inSecond = 0;

    // Audio -> message.
    std::atomic<float> fast { noneDb }, peak { noneDb };
    std::array<Second, 64> ring {};
    std::atomic<std::uint32_t> written { 0 };

    // Message thread.
    std::uint32_t read = 0;
    std::vector<Second> window = std::vector<Second> (leqSeconds);
    int head = 0, count = 0;
    float maxFast = noneDb;
};
