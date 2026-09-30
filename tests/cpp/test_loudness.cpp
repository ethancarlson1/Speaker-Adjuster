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

// ISO 226:2003 contours at the standard's 29 frequencies (20 Hz-12.5 kHz), in dB
// SPL: the standard's formula with its Table 1 parameters, to two decimals (the
// same as prototype/tests/test_loudness.py). They agree with the widely used
// public implementations of the standard. If these change, the contour math
// changed: update them only on purpose.
TEST_CASE ("ISO 226:2003 contours match the standard's")
{
    const std::vector<double> phon40 { 99.85, 93.94, 88.17, 82.63, 77.78, 73.08, 68.48, 64.37, 60.59, 56.70, 53.41, 50.40,
                                       47.58, 44.98, 43.05, 41.34, 40.06, 40.01, 41.82, 42.51, 39.23, 36.51, 35.61, 36.65,
                                       40.01, 45.83, 51.80, 54.28, 51.49 };
    const std::vector<double> phon80 { 118.99, 114.23, 109.65, 105.34, 101.72, 98.36, 95.17, 92.48, 90.09, 87.82, 85.92,
                                       84.31, 82.89, 81.68, 80.86, 80.17, 79.67, 80.01, 82.48, 83.74, 80.59, 77.88, 77.07,
                                       78.31, 81.62, 86.81, 91.41, 91.74, 85.41 };
    REQUIRE (iso226Freqs().size() == 29);
    const auto at40 = iso226ContourAtTable (40.0), at80 = iso226ContourAtTable (80.0);
    for (std::size_t i = 0; i < 29; ++i)
    {
        CAPTURE (iso226Freqs()[i]);
        CHECK (std::abs (at40[i] - phon40[i]) < 0.005);
        CHECK (std::abs (at80[i] - phon80[i]) < 0.005);
    }
    const auto at = [] (double f, double phon) { return iso226Contour ({ f }, phon).front(); };
    CHECK (at (std::sqrt (100.0 * 125.0), 40.0) == doctest::Approx ((64.37 + 60.59) / 2).epsilon (0.0002));   // log-f interpolation
    CHECK (at (10.0, 40.0) == doctest::Approx (99.85).epsilon (0.0001));                                      // held outside
    CHECK (at (16000.0, 40.0) == doctest::Approx (51.49).epsilon (0.0001));
    for (double phon : { 20.0, 40.0, 60.0, 80.0, 90.0 })
        CHECK (std::abs (at (1000.0, phon) - phon) < 0.02);   // 1 kHz defines the phon
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

TEST_CASE ("A-weighting is within IEC 61672 class 1")
{
    struct Row { double f, nominal, lo, hi; };
    const Row table[] { { 31.5, -39.4, 1.5, 1.5 }, { 63, -26.2, 1.0, 1.0 }, { 125, -16.1, 1.0, 1.0 }, { 250, -8.6, 1.0, 1.0 },
                        { 500, -3.2, 1.0, 1.0 }, { 1000, 0.0, 0.7, 0.7 }, { 2000, 1.2, 1.0, 1.0 }, { 4000, 1.0, 1.0, 1.0 },
                        { 8000, -1.1, 2.5, 1.5 } };
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto b = aWeightingBands();
        const auto g = 20.0 * std::log10 (aWeightingGain (rate));
        for (const auto& r : table)
        {
            const auto db = g + responseDb ({ b[0], b[1], b[2] }, { r.f }, rate).front();
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
        const auto target = compensationTarget (grid, 95.0, -drop);
        for (std::size_t k = 0; k < grid.size(); ++k)
            CHECK (std::abs (fit[k] - target[k]) < 0.9);
    }
    LoudnessConfig cfg;
    CHECK (shelfGains (plan, 0.0, cfg) == std::pair<double, double> { 0.0, 0.0 });
    CHECK (shelfGains (plan, 5.0, cfg) == std::pair<double, double> { 0.0, 0.0 });
    CHECK (shelfGains (plan, -30.0, cfg) == std::pair<double, double> { cfg.maxLowDb, cfg.maxHighDb });
    cfg.maxLowDb = 20.0;
    CHECK (shelfGains (plan, -40.0, cfg).first == cfg.lowCeilingDb);
    LoudnessConfig half;
    half.amount = 0.5;
    const auto full = shelfGains (plan, -7.0, LoudnessConfig {});
    CHECK (shelfGains (plan, -7.0, half).first == doctest::Approx (full.first / 2));

    // The same drop asks for nearly the same change from any usual reference: the level change sets it.
    const auto from95 = compensationTarget ({ 31.5 }, 95.0, -8.0).front(), from80 = compensationTarget ({ 31.5 }, 80.0, -8.0).front();
    CHECK (from80 == doctest::Approx (from95).epsilon (0.1));

    const auto hp = trackingHighpass (45.0, LoudnessConfig {}.maxLowDb, LoudnessConfig {});
    CHECK (hp[0].freq == doctest::Approx (45.0 * std::sqrt (2.0)));
}

TEST_CASE ("the low boost shrinks as the PA's low end nears the low shelf")
{
    const auto plan = planShelves (95.0, fs);
    const auto shelf = plan.lowFreq;
    CHECK (lowBoostAllowance (0.0, shelf) == 1.0);   // not measured
    CHECK (lowBoostAllowance (shelf / 4.0, shelf) == 1.0);
    CHECK (lowBoostAllowance (shelf / 2.0, shelf) == doctest::Approx (1.0));
    CHECK (lowBoostAllowance (shelf / std::sqrt (2.0), shelf) == doctest::Approx (0.5));
    CHECK (lowBoostAllowance (shelf, shelf) == 0.0);
    CHECK (lowBoostAllowance (2.0 * shelf, shelf) == 0.0);

    LoudnessConfig deep, smallTop, noLows;
    deep.lfLimitHz = 35.0;
    smallTop.lfLimitHz = shelf / std::sqrt (2.0);
    noLows.lfLimitHz = shelf * 1.2;
    CHECK (shelfGains (plan, -30.0, deep) == std::pair<double, double> { deep.maxLowDb, deep.maxHighDb });
    CHECK (shelfGains (plan, -30.0, smallTop).first == doctest::Approx (smallTop.maxLowDb / 2.0));
    CHECK (shelfGains (plan, -30.0, noLows) == std::pair<double, double> { 0.0, noLows.maxHighDb });
    CHECK (shelfGains (plan, -3.0, smallTop) == shelfGains (plan, -3.0, LoudnessConfig {}));   // only the ceiling moves
}

double secondsUntilWithin (const std::vector<double>& est, double target, double db)
{
    const auto it = std::find_if (est.begin(), est.end(), [&] (double v) { return std::abs (v - target) < db; });
    return it == est.end() ? 1e9 : static_cast<double> (it - est.begin()) * 512.0 / fs;
}

TEST_CASE ("tracker: steady level, pauses hold, long drops followed")
{
    LoudnessConfig slow, fast;
    fast.speedS = 5.0;
    for (const auto& cfg : { slow, fast })
    {
        LevelTracker t;
        t.prepare (fs, cfg);
        CHECK (run (t, noiseAt (20.0, -20.0, 1)).back() == doctest::Approx (-20.0).epsilon (0.005));
        CHECK_FALSE (t.isJumping());
    }

    LevelTracker t;
    t.prepare (fs, fast);
    run (t, noiseAt (10.0, -20.0, 1));
    const std::vector<float> silence (static_cast<std::size_t> (6 * fs), 0.0f);
    const auto before = t.estimate();
    run (t, silence);
    CHECK (std::abs (t.estimate() - before) < 0.2);   // only the window straddling the start of the pause moves it
    run (t, noiseAt (40.0, -45.0, 2));
    CHECK (t.estimate() == doctest::Approx (-45.0).epsilon (0.025));
}

TEST_CASE ("tracker: averaged over Speed both ways; a big jump up is followed within seconds")
{
    LoudnessConfig cfg;   // 30 s
    {
        // 4 s verses and choruses 6 dB apart: the EQ's level barely moves.
        LevelTracker t;
        t.prepare (fs, cfg);
        run (t, noiseAt (20.0, -16.5, 3));
        std::vector<double> est;
        for (int k = 0; k < 24; ++k)
            for (auto v : run (t, noiseAt (4.0, k % 2 != 0 ? -20.0 : -14.0, 10 + static_cast<unsigned> (k))))
                est.push_back (v);
        const auto tail = std::vector<double> (est.begin() + static_cast<long> (30 * fs / 512), est.end());
        CHECK (*std::max_element (tail.begin(), tail.end()) - *std::min_element (tail.begin(), tail.end()) < 1.0);
        CHECK_FALSE (t.isJumping());
    }
    {
        // 4 dB up (under the guard) takes the long average; 6 dB down too.
        LevelTracker t;
        t.prepare (fs, cfg);
        run (t, noiseAt (20.0, -20.0, 4));
        const auto up = secondsUntilWithin (run (t, noiseAt (60.0, -16.0, 5)), -16.0, 1.0);
        CHECK (up > 20.0);
        CHECK (up < 50.0);
        const auto down = secondsUntilWithin (run (t, noiseAt (90.0, -22.0, 6)), -22.0, 1.0);
        CHECK (down > 40.0);
        CHECK (down < 70.0);
    }
    {
        // A loud song after a ballad: 12 dB up is followed within seconds.
        LevelTracker t;
        t.prepare (fs, cfg);
        run (t, noiseAt (60.0, -30.0, 7));
        const auto up = run (t, noiseAt (40.0, -18.0, 8));
        CHECK (secondsUntilWithin (up, -18.0, 3.0) < 5.0);
        CHECK (secondsUntilWithin (up, -18.0, 1.0) < 40.0);
        CHECK_FALSE (t.isJumping());
    }
    {
        // A loud moment in a song (8 dB up for 2 s) doesn't trip it.
        LevelTracker t;
        t.prepare (fs, cfg);
        run (t, noiseAt (30.0, -30.0, 9));
        auto x = noiseAt (2.0, -22.0, 10);
        const auto after = noiseAt (10.0, -30.0, 11);
        x.insert (x.end(), after.begin(), after.end());
        const auto est = run (t, x);
        CHECK (*std::max_element (est.begin(), est.end()) < -28.5);
    }
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
