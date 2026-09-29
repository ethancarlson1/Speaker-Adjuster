#include "plugin/ZoneStage.h"

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 256;
constexpr double pi = 3.14159265358979323846;

// Runs x (the same on both channels) through the stage in blocks; returns the left output.
// `settingsAt` gives the setting for each block's first sample.
template <typename Settings>
std::vector<float> run (ZoneStage& stage, const std::vector<float>& x, Settings settingsAt, std::vector<float>* right = nullptr)
{
    std::vector<float> l (x), r (x);
    for (std::size_t pos = 0; pos < x.size(); pos += block)
    {
        const auto n = static_cast<int> (std::min<std::size_t> (block, x.size() - pos));
        float* ch[] = { l.data() + pos, r.data() + pos };
        stage.process (ch, 2, n, settingsAt (pos));
    }
    if (right != nullptr)
        *right = r;
    return l;
}

std::vector<float> sine (double freq, double seconds, double delaySamples = 0.0)
{
    std::vector<float> x (static_cast<std::size_t> (seconds * fs));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (0.5 * std::sin (2.0 * pi * freq * (static_cast<double> (i) - delaySamples) / fs));
    return x;
}
} // namespace

TEST_CASE ("zone stage: 0 ms and normal polarity pass the audio unchanged")
{
    ZoneStage stage;
    stage.prepare (fs);
    const auto x = sine (997.0, 0.5);
    const auto y = run (stage, x, [] (std::size_t) { return ZoneSettings {}; });
    CHECK (y == x);
}

TEST_CASE ("zone stage: whole-sample delay and polarity")
{
    ZoneStage stage;
    stage.prepare (fs, { 10.0, true });    // 480 samples, inverted
    std::vector<float> x (2000, 0.0f);
    x[100] = 1.0f;
    std::vector<float> right;
    const auto y = run (stage, x, [] (std::size_t) { return ZoneSettings { 10.0, true }; }, &right);
    CHECK (y[580] == -1.0f);
    CHECK (right[580] == -1.0f);
    auto energy = 0.0;
    for (auto v : y)
        energy += static_cast<double> (v) * v;
    CHECK (energy == doctest::Approx (1.0));
    CHECK (ZoneStage::delaySamples (10.0, fs) == doctest::Approx (480.0));
    CHECK (ZoneStage::delaySamples (1000.0, fs) == doctest::Approx (ZoneStage::maxDelayMs * 48.0));   // clamped
}

TEST_CASE ("zone stage: fractional delays are exact and flat")
{
    for (const auto ms : { 12.345, 0.51, 299.99 })
    {
        CAPTURE (ms);
        ZoneStage stage;
        stage.prepare (fs, { ms, false });
        const auto d = ZoneStage::delaySamples (ms, fs);
        for (const auto freq : { 100.0, 1000.0, 15000.0 })
        {
            CAPTURE (freq);
            stage.prepare (fs, { ms, false });
            const auto y = run (stage, sine (freq, 0.8), [ms] (std::size_t) { return ZoneSettings { ms, false }; });
            const auto expected = sine (freq, 0.8, d);
            auto worst = 0.0;
            for (std::size_t i = static_cast<std::size_t> (d) + 64; i < y.size(); ++i)
                worst = std::max (worst, std::abs (static_cast<double> (y[i]) - expected[i]));
            CHECK (worst < 2e-4);   // -68 dB against a -6 dBFS sine: right timing, flat level
        }
    }
    // Below 15 samples the delay is the nearest whole sample.
    ZoneStage stage;
    stage.prepare (fs, { 0.01, false });   // 0.48 samples -> 0
    const auto x = sine (997.0, 0.2);
    CHECK (run (stage, x, [] (std::size_t) { return ZoneSettings { 0.01, false }; }) == x);
}

TEST_CASE ("zone stage: changes crossfade over 20 ms, and one mid-fade waits its turn")
{
    ZoneStage stage;
    stage.prepare (fs);
    const std::vector<float> dc (static_cast<std::size_t> (0.3 * fs), 0.5f);
    // Flip the polarity about 0.1 s in (settings change per block), then ask for a delay one block later (mid-fade).
    const std::size_t flipAt = block * 19;
    const auto moveAt = flipAt + block;
    const auto y = run (stage, dc, [&] (std::size_t pos)
    {
        if (pos < flipAt)
            return ZoneSettings {};
        return pos < moveAt ? ZoneSettings { 0.0, true } : ZoneSettings { 3.0, true };
    });
    const auto fade = static_cast<std::size_t> (ZoneStage::fadeSeconds * fs);
    CHECK (y[flipAt - 1] == 0.5f);
    auto biggestStep = 0.0f;
    for (std::size_t i = 1; i < y.size(); ++i)
        biggestStep = std::max (biggestStep, std::abs (y[i] - y[i - 1]));
    CHECK (biggestStep < 1.1f / static_cast<float> (fade));   // a straight 20 ms line from +0.5 to -0.5
    CHECK (y[flipAt + fade / 2] == doctest::Approx (0.0).epsilon (0.01));
    CHECK (y[flipAt + fade] == -0.5f);
    CHECK (y.back() == -0.5f);                                  // DC doesn't care about the delay...
    CHECK (stage.getCurrent() == ZoneSettings { 3.0, true });   // ...but it got there
    CHECK_FALSE (stage.isFading());
}

TEST_CASE ("zone stage: mono")
{
    ZoneStage stage;
    stage.prepare (fs, { 1.0, false });
    std::vector<float> x (1000, 0.0f);
    x[10] = 1.0f;
    float* ch[] = { x.data() };
    stage.process (ch, 1, static_cast<int> (x.size()), { 1.0, false });
    CHECK (x[58] == 1.0f);
}

TEST_CASE ("zone delay text gives the distance")
{
    CHECK (zoneDelayText (12.5) == "12.50 ms (4.29 m / 14.1 ft)");
    CHECK (zoneDelayText (0.0) == "0.00 ms (0.00 m / 0.0 ft)");
}

TEST_CASE ("typed delays: ms, or a distance")
{
    CHECK (parseZoneDelay ("12.5") == doctest::Approx (12.5));
    CHECK (parseZoneDelay ("12,5 ms") == doctest::Approx (12.5));
    CHECK (parseZoneDelay (zoneDelayText (12.5)) == doctest::Approx (12.5));   // the parameter's own text round-trips
    CHECK (parseZoneDelay ("34.3 m") == doctest::Approx (100.0));
    CHECK (parseZoneDelay ("343cm") == doctest::Approx (10.0));
    CHECK (parseZoneDelay ("10 FT") == doctest::Approx (3.048 / 343.0 * 1000.0));
    CHECK (parseZoneDelay ("1000") == doctest::Approx (ZoneStage::maxDelayMs));
    CHECK (parseZoneDelay ("-5") == doctest::Approx (5.0));   // no negative delays: the sign is ignored
    CHECK (parseZoneDelay ("abc") == 0.0);
}
