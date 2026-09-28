// Drives the real-time recorder the way an audio callback would: block by
// block, with a simulated loop (the speaker's output reaches the mic through a
// filter and a delay), then analyses what it recorded.

#include "helpers.h"
#include "plugin/CaptureRecorder.h"
#include "roomeq/capture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <deque>

using namespace testing;

namespace
{
// Streaming loop: mic[n] = filter(speaker[n - delay]) + noise.
struct Loop
{
    Biquad hp = highpass (80.0, 0.7071, 48000.0);
    Biquad peq = peaking (1000.0, -6.0, 1.0, 48000.0);
    double hpS1 = 0, hpS2 = 0, peqS1 = 0, peqS2 = 0;
    std::deque<double> line = std::deque<double> (480, 0.0);   // 10 ms
    std::vector<double> noise = whiteNoise (1 << 20, 1e-5, 7);
    std::size_t n = 0;

    static double tick (const Biquad& b, double x, double& s1, double& s2)
    {
        const auto y = b.b0 * x + s1;
        s1 = b.b1 * x - b.a1 * y + s2;
        s2 = b.b2 * x - b.a2 * y;
        return y;
    }

    float process (float speaker)
    {
        line.push_back (tick (peq, tick (hp, speaker, hpS1, hpS2), peqS1, peqS2));
        const auto out = line.front();
        line.pop_front();
        return static_cast<float> (out + noise[n++ % noise.size()]);
    }
};

struct Run
{
    std::vector<float> left, right;   // what the plugin sent to each speaker
};

// Runs blocks until the recorder finishes (or maxBlocks). speakerChannel is the
// channel the loop listens to. `input` feeds the main bus (program).
Run drive (CaptureRecorder& rec, Loop& loop, int speakerChannel, int blockSize, int maxBlocks,
           const std::vector<float>* input = nullptr, int cancelAfterBlocks = -1)
{
    Run out;
    std::vector<float> l (static_cast<std::size_t> (blockSize)), r (static_cast<std::size_t> (blockSize)),
        mic (static_cast<std::size_t> (blockSize));
    float* main[] = { l.data(), r.data() };
    std::vector<float> pendingSpeaker (static_cast<std::size_t> (blockSize), 0.0f);
    std::size_t inputPos = 0;
    for (int b = 0; b < maxBlocks && rec.getCurrent() != nullptr && ! rec.getCurrent()->finished.load(); ++b)
    {
        if (b == cancelAfterBlocks)
            rec.cancel();
        // The mic hears last block's output (a 1-block interface latency on top of the loop).
        for (std::size_t i = 0; i < mic.size(); ++i)
            mic[i] = loop.process (pendingSpeaker[i]);
        for (std::size_t i = 0; i < l.size(); ++i)
        {
            const auto x = input != nullptr && inputPos < input->size() ? (*input)[inputPos++] : 0.0f;
            l[i] = x;
            r[i] = x;
        }
        rec.process (main, 2, mic.data(), blockSize);
        pendingSpeaker.assign (main[speakerChannel], main[speakerChannel] + blockSize);
        out.left.insert (out.left.end(), l.begin(), l.end());
        out.right.insert (out.right.end(), r.begin(), r.end());
    }
    return out;
}
} // namespace

TEST_CASE ("sweep plays on one speaker only and the recording analyses cleanly")
{
    for (auto channel : { 0, 1 })
    {
        CaptureRecorder rec;
        REQUIRE (rec.start (makeSweepRequest (48000.0, 2.0, 2, channel, -12.0)));
        CHECK_FALSE (rec.start (makeSweepRequest (48000.0, 2.0, 1, 0, -12.0)));   // one at a time

        Loop loop;
        const auto run = drive (rec, loop, channel, 512, 100000);
        auto finished = rec.collectFinished();
        REQUIRE (finished != nullptr);
        CHECK_FALSE (finished->cancelled);
        CHECK_FALSE (rec.isBusy());

        const auto& silent = channel == 0 ? run.right : run.left;
        const auto& swept = channel == 0 ? run.left : run.right;
        CHECK (std::all_of (silent.begin(), silent.end(), [] (float v) { return v == 0.0f; }));
        CHECK (*std::max_element (swept.begin(), swept.end()) > 0.2f);

        std::vector<std::vector<double>> takes;
        for (const auto& m : finished->mic)
            takes.emplace_back (m.begin(), m.end());
        const auto c = roomeq::analyzeSweepCapture ("P1", takes, finished->sweepConfig);
        CHECK (c.grade.overall == roomeq::Grade::pass);
        // Loop delay = 10 ms line + one 512-sample block + the filters' peak.
        CHECK (c.delaysMs[0] == doctest::Approx (1000.0 * (480 + 512) / 48000.0).epsilon (0.01));
        for (auto f : geomspace (100.0, 16000.0, 40))
        {
            const auto k = static_cast<std::size_t> (std::lround (f / c.freqs[1]));
            const auto expected = loop.hp.magnitudeDb (f, 48000.0) + loop.peq.magnitudeDb (f, 48000.0);
            CHECK (std::abs (10.0 * std::log10 (c.power[k]) - expected) < 0.25);
        }
    }
}

TEST_CASE ("cancel silences the output and hands the request back")
{
    CaptureRecorder rec;
    REQUIRE (rec.start (makeSweepRequest (48000.0, 5.0, 1, 0, -12.0)));
    Loop loop;
    const auto run = drive (rec, loop, 0, 256, 100000, nullptr, 40);
    auto finished = rec.collectFinished();
    REQUIRE (finished != nullptr);
    CHECK (finished->cancelled);
    CHECK (run.left.size() == 41 * 256);   // stopped in the block that saw the request
    CHECK (std::all_of (run.left.end() - 256, run.left.end(), [] (float v) { return v == 0.0f; }));
}

TEST_CASE ("program capture passes audio through and records input and mic")
{
    CaptureRecorder rec;
    REQUIRE (rec.start (makeProgramRequest (48000.0, 3.0)));
    const auto noise = whiteNoise (static_cast<std::size_t> (4 * 48000), 0.1, 11);
    const std::vector<float> program (noise.begin(), noise.end());
    Loop loop;
    const auto run = drive (rec, loop, 0, 480, 100000, &program);
    auto finished = rec.collectFinished();
    REQUIRE (finished != nullptr);
    CHECK_FALSE (finished->cancelled);
    for (std::size_t i = 0; i < 1000; ++i)
        CHECK (run.left[i] == program[i]);   // untouched
    for (std::size_t i = 0; i < finished->reference.size(); i += 997)
        CHECK (finished->reference[i] == doctest::Approx (program[i]));
    CHECK (std::abs (finished->mic[0][48000]) > 0.0f);
}

TEST_CASE ("abortWhileStopped releases a request that never ran")
{
    CaptureRecorder rec;
    REQUIRE (rec.start (makeSweepRequest (48000.0, 2.0, 1, 0, -12.0)));
    rec.abortWhileStopped();
    auto finished = rec.collectFinished();
    REQUIRE (finished != nullptr);
    CHECK (finished->cancelled);
    CHECK (rec.start (makeSweepRequest (48000.0, 2.0, 1, 0, -12.0)));
}
