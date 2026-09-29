#include "helpers.h"
#include "plugin/LoudnessStage.h"

#include <doctest/doctest.h>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;
constexpr int block = 512;

std::vector<float> noiseAt (double seconds, double levelDbfsC, unsigned seed)
{
    auto x = testing::whiteNoise (static_cast<std::size_t> (seconds * fs), 1.0, seed);
    const auto gain = std::pow (10.0, (levelDbfsC - cWeightedLevelDbfs (x, fs)) / 20.0);
    std::vector<float> out (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        out[i] = static_cast<float> (x[i] * gain);
    return out;
}

// Runs x (same on both channels) through the stage; returns the left output.
std::vector<double> run (LoudnessStage& stage, const std::vector<float>& x, const LoudnessSettings& s,
                         bool suspend = false, const std::vector<float>* mic = nullptr, bool snap = false)
{
    std::vector<float> l (x), r (x);
    for (std::size_t pos = 0; pos + block <= x.size(); pos += block)
    {
        float* ch[] = { l.data() + pos, r.data() + pos };
        stage.process (ch, 2, mic != nullptr ? mic->data() + pos : nullptr, block, s, suspend, snap);
        stage.recordTap (ch, 2, mic != nullptr ? mic->data() + pos : nullptr, block);
    }
    return { l.begin(), l.end() };
}

std::vector<float> impulseAfterSilence()
{
    std::vector<float> x (static_cast<std::size_t> (3 * fs), 0.0f);   // silence: the tracker holds, glides settle
    x[static_cast<std::size_t> (2 * fs)] = 1.0f;
    return x;
}

LoudnessModel calibratedModel()
{
    LoudnessModel m;
    m.plan = planShelves (95.0, fs);
    m.hasPlan = true;
    m.calibration = { -20.0, 95.0, true, -32.0 };   // -20 dBFS(C) out = 95 dB(C); the mic read -32 then
    m.calibrated = true;
    return m;
}
} // namespace

TEST_CASE ("loudness stage: nothing happens until calibrated")
{
    LoudnessStage stage;
    LoudnessSettings s;
    stage.prepare (fs, block, s);
    const auto x = noiseAt (5.0, -30.0, 1);
    const auto y = run (stage, x, s);
    for (std::size_t i = 0; i < x.size(); i += 97)
        REQUIRE (y[i] == static_cast<double> (x[i]));
    CHECK_FALSE (stage.getStatus().hasLevel.load());
}

TEST_CASE ("loudness stage: 10 dB below the reference gets the planned shelves")
{
    LoudnessStage stage;
    LoudnessSettings s;
    stage.setModel (calibratedModel());
    stage.prepare (fs, block, s);
    run (stage, noiseAt (30.0, -30.0, 2), s);                  // 85 dB(C) at the mix position
    const auto& st = stage.getStatus();
    REQUIRE (st.hasLevel.load());
    CHECK (st.splUsed.load() == doctest::Approx (85.0).epsilon (0.01));
    const auto plan = planShelves (95.0, fs);
    const auto [low, high] = shelfGains (plan, st.splUsed.load(), s.config);
    CHECK (st.lowGainDb.load() == doctest::Approx (low));
    CHECK (low > 4.5);

    const auto y = run (stage, impulseAfterSilence(), s);
    const std::vector<double> ir (y.begin() + static_cast<long> (2 * fs), y.end());
    const std::vector<Band> expected { { BandKind::lowShelf, plan.lowFreq, low, plan.lowQ },
                                       { BandKind::highShelf, plan.highFreq, high, plan.highQ } };
    for (double f : { 40.0, 100.0, 1000.0, 5000.0, 12000.0 })
        CHECK (testing::dtftDb (ir, f, fs) == doctest::Approx (responseDb (expected, { f }, fs).front()).epsilon (0.003));

    // A measurement running: flat, and the level isn't touched.
    const auto before = st.splUsed.load();
    const auto z = run (stage, impulseAfterSilence(), s, true);
    const std::vector<double> flat (z.begin() + static_cast<long> (2 * fs), z.end());
    CHECK (std::abs (testing::dtftDb (flat, 100.0, fs)) < 0.01);   // glided to flat
    run (stage, noiseAt (5.0, -10.0, 3), s, true);
    CHECK (st.splUsed.load() == before);
}

TEST_CASE ("loudness stage: a test signal takes it flat at once, music glides")
{
    for (const auto snap : { true, false })
    {
        LoudnessStage stage;
        LoudnessSettings s;
        s.config.hpTrack = true;
        s.hpBaseHz = 45.0;
        stage.setModel (calibratedModel());
        stage.prepare (fs, block, s);
        run (stage, noiseAt (30.0, -30.0, 6), s);             // 85 dB(C): shelves up, high-pass on
        REQUIRE (stage.getStatus().lowGainDb.load() > 4.5f);

        std::vector<float> x (static_cast<std::size_t> (fs), 0.0f);
        x[3000] = 1.0f;
        const auto y = run (stage, x, s, true, nullptr, snap);
        const std::vector<double> ir (y.begin() + 3000, y.end());
        const auto error = std::abs (testing::dtftDb (ir, 50.0, fs));
        if (snap)
        {
            auto exact = true;
            for (std::size_t i = 0; i < x.size(); ++i)
                exact = exact && y[i] == static_cast<double> (x[i]);
            CHECK (exact);                                    // untouched from the first sample
        }
        else
        {
            CHECK (error > 0.5);                              // still gliding 60 ms in
        }
    }
}

TEST_CASE ("loudness stage: at or above the reference it stays flat; the high-pass tracks")
{
    LoudnessStage stage;
    LoudnessSettings s;
    s.config.speedS = 5.0;   // the fastest Speed: the 22 dB drop below is followed within the 40 s
    s.config.hpTrack = true;
    s.hpBaseHz = 45.0;
    stage.setModel (calibratedModel());
    stage.prepare (fs, block, s);
    run (stage, noiseAt (20.0, -18.0, 4), s);                  // 97 dB(C): above the reference
    CHECK (stage.getStatus().lowGainDb.load() == 0.0f);
    CHECK (stage.getStatus().hpFreq.load() == doctest::Approx (45.0));
    run (stage, noiseAt (40.0, -40.0, 5), s);                  // 75 dB(C): max low boost
    CHECK (stage.getStatus().lowGainDb.load() == doctest::Approx (s.config.maxLowDb));
    CHECK (stage.getStatus().hpFreq.load() == doctest::Approx (45.0 * std::sqrt (2.0)).epsilon (0.001));
}

TEST_CASE ("loudness stage: mic source and the tap")
{
    LoudnessStage stage;
    LoudnessSettings s;
    s.useMic = true;
    stage.setModel (calibratedModel());
    stage.prepare (fs, block, s);
    const auto out = noiseAt (20.0, -20.0, 6);
    std::vector<float> mic (out.size());
    for (std::size_t i = 0; i < mic.size(); ++i)
        mic[i] = out[i] * 0.125f;                              // 18 dB down: 6 dB quieter than at calibration
    auto tap = std::make_unique<TapRequest>();
    tap->skip = 1000;
    tap->output.resize (4800);
    tap->mic.resize (4800);
    REQUIRE (stage.startTap (std::move (tap)));
    run (stage, out, s, false, &mic);
    CHECK (stage.getStatus().splNow.load() == doctest::Approx (89.0).epsilon (0.005));
    const auto done = stage.collectTap();
    REQUIRE (done != nullptr);
    CHECK_FALSE (done->cancelled);
    CHECK (done->mic[10] == mic[1010]);
    CHECK_FALSE (stage.isTapBusy());
}
