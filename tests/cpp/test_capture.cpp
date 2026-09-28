#include "helpers.h"
#include "roomeq/averaging.h"
#include "roomeq/capture.h"

#include <doctest/doctest.h>

#include <stdexcept>

using namespace roomeq;
using namespace testing;

namespace
{
// A small synthetic "room": PA-like filters, then a direct sound plus a few
// reflections and a decaying diffuse tail, then latency.
struct SyntheticRoom
{
    double fs = 48000.0;
    std::size_t latency = 480;   // 10 ms
    Biquad hp = highpass (60.0, 0.7071, 48000.0);
    Biquad bump = peaking (150.0, 3.0, 0.9, 48000.0);
    Biquad dip = peaking (2800.0, -4.0, 1.8, 48000.0);
    std::vector<double> room;

    SyntheticRoom()
    {
        room.assign (static_cast<std::size_t> (0.8 * fs), 0.0);
        room[0] = 1.0;
        room[371] = -0.45;
        room[912] = 0.3;
        room[1645] = 0.2;
        const auto tail = whiteNoise (room.size(), 0.02, 9);
        for (std::size_t i = 2000; i < room.size(); ++i)
            room[i] += tail[i] * std::exp (-6.9 * static_cast<double> (i) / (0.6 * fs));   // RT60 ~ 0.6 s
    }

    std::vector<double> linear (const std::vector<double>& x) const
    {
        return delayed (fftConvolve (dip.process (bump.process (hp.process (x))), room), latency);
    }

    std::vector<double> impulseResponse() const
    {
        std::vector<double> imp (static_cast<std::size_t> (fs), 0.0);
        imp[0] = 1.0;
        return linear (imp);
    }
};

std::vector<double> plus (std::vector<double> a, const std::vector<double>& b)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] += b[i];
    return a;
}
} // namespace

TEST_CASE ("sweep capture matches the true response and detects the delay")
{
    SyntheticRoom r;
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto play = buildPlayback (cfg, generateSweep (cfg));
    const auto clean = r.linear (play);
    const std::vector<std::vector<double>> recs { plus (clean, whiteNoise (clean.size(), 1e-4, 1)),
                                                  plus (clean, whiteNoise (clean.size(), 1e-4, 2)) };
    const auto c = analyzeSweepCapture ("P1", recs, cfg);

    const auto truth = windowedResponse (r.impulseResponse(), cfg.fs);
    REQUIRE (c.delaysMs.size() == 2);
    CHECK (c.delaysMs[0] == doctest::Approx (1000.0 * static_cast<double> (truth.peak) / cfg.fs));
    CHECK (c.delaysMs[0] == doctest::Approx (c.delaysMs[1]));
    CHECK (c.grade.overall == Grade::pass);

    const auto grid = logFreqGrid (30.0, 16000.0, 24);
    const auto measured = toDb (smoothPower (c.freqs, c.power, 6.0, grid));
    const auto expected = toDb (smoothPower (truth.freqs, truth.power, 6.0, grid));
    for (std::size_t i = 0; i < grid.size(); ++i)
        CHECK (std::abs (measured[i] - expected[i]) < 0.1);
}

TEST_CASE ("noise below 100 Hz is graded and reported")
{
    SyntheticRoom r;
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto play = buildPlayback (cfg, generateSweep (cfg));
    auto rumble = whiteNoise (play.size(), 1.0, 3);
    for (int pass = 0; pass < 4; ++pass)
        rumble = lowpass (70.0, 0.7071, cfg.fs).process (rumble);
    const auto c = analyzeSweepCapture ("P1", { plus (r.linear (play), rumble) }, cfg);
    CHECK (c.grade.overall == Grade::redo);
    REQUIRE_FALSE (c.grade.reasons.empty());
    CHECK (c.grade.reasons[0].rfind ("redo: low-end noise too high below", 0) == 0);
}

TEST_CASE ("a tail too short for the loop delay is an error")
{
    SweepConfig cfg;
    cfg.duration = 2.0;
    cfg.tail = 0.4;
    const auto play = buildPlayback (cfg, generateSweep (cfg));
    CHECK_THROWS_AS (analyzeSweepCapture ("P1", { delayed (play, 4800) }, cfg), std::runtime_error);
}

TEST_CASE ("program capture matches the sweep capture")
{
    SyntheticRoom r;
    const auto fs = r.fs;
    // Pink-ish "program": white noise through a gentle low-pass tilt.
    auto x = whiteNoise (static_cast<std::size_t> (15 * fs), 0.1, 5);
    x = lowpass (3000.0, 0.5, fs).process (x);
    const auto prog = analyzeProgramCapture ("W", x, r.linear (x), fs);

    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto sweepCap = analyzeSweepCapture ("P1", { r.linear (buildPlayback (cfg, generateSweep (cfg))) }, cfg);

    const auto grid = logFreqGrid (100.0, 10000.0, 24);
    const auto a = toDb (smoothPower (prog.freqs, prog.power, 3.0, grid, &prog.weight));
    const auto b = toDb (smoothPower (sweepCap.freqs, sweepCap.power, 3.0, grid));
    for (std::size_t i = 0; i < grid.size(); ++i)
        CHECK (std::abs (a[i] - b[i]) < 1.0);
    CHECK (prog.delaysMs.at (0) == doctest::Approx (sweepCap.delaysMs.at (0)).epsilon (0.01));
}
