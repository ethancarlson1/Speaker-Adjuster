#include "helpers.h"
#include "roomeq/iso226.h"
#include "roomeq/loudness.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;

// Pink-ish noise at a given C-weighted level (white noise through a -3 dB/oct tilt is
// enough here: the tracker only cares about level).
std::vector<float> noiseAt (double seconds, double levelDbfsC, unsigned seed)
{
    auto x = testing::whiteNoise (static_cast<std::size_t> (seconds * fs), 1.0, seed);
    const auto gain = std::pow (10.0, (levelDbfsC - cWeightedLevelDbfs (x, fs)) / 20.0);
    std::vector<float> out (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        out[i] = static_cast<float> (x[i] * gain);
    return out;
}

// Runs blocks through the tracker; returns the estimate after each block (NaN before the first).
std::vector<double> run (LevelTracker& t, const std::vector<float>& x, int block = 512)
{
    std::vector<double> out;
    for (std::size_t i = 0; i + static_cast<std::size_t> (block) <= x.size(); i += static_cast<std::size_t> (block))
    {
        t.process (x.data() + i, block);
        out.push_back (t.hasEstimate() ? t.estimate() : std::nan (""));
    }
    return out;
}
} // namespace

TEST_CASE ("ISO 226 contours match the published values")
{
    const auto at = [] (double f, double phon) { return iso226Contour ({ f }, phon).front(); };
    CHECK (at (20.0, 40.0) == doctest::Approx (99.85).epsilon (0.0006));
    CHECK (at (20.0, 60.0) == doctest::Approx (109.51).epsilon (0.0005));
    CHECK (at (20.0, 80.0) == doctest::Approx (118.99).epsilon (0.0005));
    for (double phon : { 20.0, 40.0, 60.0, 80.0, 90.0 })
        CHECK (at (1000.0, phon) == doctest::Approx (phon).epsilon (0.002));
}

TEST_CASE ("C-weighting is within IEC 61672 class 1")
{
    struct Row { double f, nominal, lo, hi; };
    const Row table[] { { 31.5, -3.0, 1.5, 1.5 }, { 63, -0.8, 1.0, 1.0 }, { 125, -0.2, 1.0, 1.0 }, { 1000, 0.0, 0.7, 0.7 },
                        { 4000, -0.8, 1.0, 1.0 }, { 8000, -3.0, 2.5, 1.5 } };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto bands = cWeightingBands();
        const auto g = 20.0 * std::log10 (cWeightingGain (rate));
        for (const auto& r : table)
        {
            const auto db = g + responseDb ({ bands[0], bands[1] }, { r.f }, rate).front();
            CHECK (db >= r.nominal - r.lo);
            CHECK (db <= r.nominal + r.hi);
        }
    }
}

TEST_CASE ("shelf plan follows the ISO 226 difference; limits and ceiling")
{
    const auto plan = planShelves (95.0, fs);
    CHECK (plan.lowFreq > 100.0);
    CHECK (plan.lowFreq < 180.0);
    CHECK (plan.highFreq > 8000.0);
    const auto grid = testing::geomspace (30.0, 16000.0, 120);
    for (double drop : { 5.0, 10.0, 15.0, 20.0 })
    {
        const auto i = static_cast<std::size_t> (drop);
        const auto fit = responseDb ({ { BandKind::lowShelf, plan.lowFreq, plan.lowGain[i], plan.lowQ },
                                       { BandKind::highShelf, plan.highFreq, plan.highGain[i], plan.highQ } },
                                     grid, fs);
        const auto target = compensationTarget (grid, 95.0 - drop, 95.0);
        for (std::size_t k = 0; k < grid.size(); ++k)
            CHECK (std::abs (fit[k] - target[k]) < 0.9);
    }
    LoudnessConfig cfg;
    CHECK (shelfGains (plan, 95.0, cfg) == std::pair<double, double> { 0.0, 0.0 });
    CHECK (shelfGains (plan, 100.0, cfg) == std::pair<double, double> { 0.0, 0.0 });
    CHECK (shelfGains (plan, 65.0, cfg) == std::pair<double, double> { cfg.maxLowDb, cfg.maxHighDb });
    cfg.maxLowDb = 20.0;
    CHECK (shelfGains (plan, 55.0, cfg).first == cfg.lowCeilingDb);
    LoudnessConfig half;
    half.amount = 0.5;
    const auto full = shelfGains (plan, 88.0, LoudnessConfig {});
    CHECK (shelfGains (plan, 88.0, half).first == doctest::Approx (full.first / 2));

    const auto hp = trackingHighpass (45.0, LoudnessConfig {}.maxLowDb, LoudnessConfig {});
    CHECK (hp[0].freq == doctest::Approx (45.0 * std::sqrt (2.0)));
}

TEST_CASE ("tracker: steady level, pauses hold, long drops followed, rises fast")
{
    LoudnessConfig cfg;
    LevelTracker t;
    t.prepare (fs, cfg);
    auto est = run (t, noiseAt (10.0, -20.0, 1));
    CHECK (est.back() >= -20.1);
    CHECK (est.back() < -19.6);

    const std::vector<float> silence (static_cast<std::size_t> (6 * fs), 0.0f);
    const auto before = t.estimate();
    run (t, silence);
    CHECK (std::abs (t.estimate() - before) < 0.2);   // only the window straddling the start of the pause moves it
    run (t, noiseAt (40.0, -45.0, 2));
    CHECK (t.estimate() == doctest::Approx (-45.0).epsilon (0.025));

    LevelTracker u;
    u.prepare (fs, cfg);
    run (u, noiseAt (10.0, -20.0, 3));
    const auto up = run (u, noiseAt (6.0, -14.0, 4));
    const auto first = std::find_if (up.begin(), up.end(), [] (double v) { return std::abs (v + 14.0) < 1.0; }) - up.begin();
    CHECK (static_cast<double> (first) * 512.0 / fs < 2.5);
}

TEST_CASE ("deadband: small wobbles barely move it, a real change lands")
{
    LoudnessConfig cfg;
    Deadband d;
    CHECK (d.update (-20.0, cfg) == -20.0);
    CHECK (d.update (-21.5, cfg) > -20.1);            // inside the band: slow drift only
    CHECK (d.update (-26.0, cfg) == -24.0);           // pushed: follows, 2 dB behind
    double held = 0.0;
    for (int i = 0; i < 400; ++i)
        held = d.update (-26.0, cfg);
    CHECK (held == doctest::Approx (-26.0).epsilon (0.001));
}

TEST_CASE ("re-check: a gain change after the plugin is found; too little program is refused")
{
    // "Room": delay + a gentle filter; the gain after the plugin changes by +4 dB.
    const auto x = testing::whiteNoise (static_cast<std::size_t> (12 * fs), 0.1, 5);
    const auto room = testing::peaking (300.0, 4.0, 1.0, fs);
    auto y = testing::delayed (room.process (x), 400);
    const auto cal = transferBandsDb (x, y, fs);
    CHECK (std::count_if (cal.begin(), cal.end(), [] (double v) { return std::isfinite (v); }) >= 20);
    for (auto& v : y)
        v *= std::pow (10.0, 4.0 / 20.0);
    const auto crowd = testing::whiteNoise (y.size(), 0.1, 6);
    for (std::size_t i = 0; i < y.size(); ++i)
        y[i] += 0.3 * crowd[i];                        // uncorrelated with the output
    const auto change = recheckGainChange (cal, transferBandsDb (x, y, fs));
    REQUIRE (change.has_value());
    CHECK (*change == doctest::Approx (4.0).epsilon (0.1));

    std::vector<double> few (recheckBands().size(), std::nan (""));
    few[0] = few[1] = few[2] = 1.0;
    CHECK_FALSE (recheckGainChange (cal, few).has_value());

    Calibration c { -22.0, 95.0 };
    CHECK (recalibrated (c, 4.0).splFromOutput (-22.0) == doctest::Approx (99.0));
}
