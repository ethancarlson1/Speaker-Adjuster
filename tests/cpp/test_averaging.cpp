#include "helpers.h"
#include "roomeq/averaging.h"
#include "roomeq/capture.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace roomeq;

namespace
{
const auto freqs = rfftFreqs (32768, 48000.0);
const std::vector<double> ones (freqs.size(), 1.0);

std::vector<double> shape()
{
    std::vector<double> p (freqs.size());
    for (std::size_t k = 0; k < p.size(); ++k)
        p[k] = 1.0 + 0.5 * std::cos (freqs[k] / 500.0);
    return p;
}

std::vector<double> scaled (std::vector<double> v, double gain)
{
    for (auto& x : v)
        x *= gain;
    return v;
}

std::shared_ptr<const Capture> fakeCapture (const std::vector<double>& power, bool excluded = false, double fs = 48000.0)
{
    auto c = std::make_shared<Capture>();
    c->fs = fs;
    c->freqs = rfftFreqs (2 * (power.size() - 1), fs);
    c->power = power;
    c->weight = inBand (c->freqs, 20.0, 20000.0);
    c->grade = gradeCapture (c->freqs, power, std::vector<double> (power.size(), 1e-6), nullptr, {}, 20000.0);
    c->excluded = excluded;
    return c;
}
} // namespace

TEST_CASE ("power average of identical spectra is unchanged")
{
    const auto p = shape();
    const auto r = powerAverage (freqs, { p, p, p }, { ones, ones, ones });
    for (std::size_t k = 0; k < p.size(); ++k)
        CHECK (r.power[k] == doctest::Approx (p[k]));
}

TEST_CASE ("averages power, not dB")
{
    const auto r = powerAverage (freqs, { ones, scaled (ones, 4.0) }, { ones, ones }, false);
    CHECK (r.power[1000] == doctest::Approx (2.5));
}

TEST_CASE ("level alignment removes a distance offset")
{
    const auto p = shape();
    const auto r = powerAverage (freqs, { p, scaled (p, std::pow (10.0, 0.6)) }, { ones, ones });
    CHECK (r.offsetsDb[0] == doctest::Approx (3.0).epsilon (0.01));
    CHECK (r.offsetsDb[1] == doctest::Approx (-3.0).epsilon (0.01));
}

TEST_CASE ("redo bands are masked out of the average")
{
    std::vector<double> noise (freqs.size(), 1e-4);
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (freqs[k] < 88.0)
            noise[k] = 0.5;
    const auto bad = gradeCapture (freqs, ones, noise, nullptr, {}, 20000.0);
    const auto w = bandMaskWeights (freqs, bad);
    const auto at = [&] (double f) { return static_cast<std::size_t> (std::lround (f / freqs[1])); };
    CHECK (w[at (40.0)] == 0.0);
    CHECK (w[at (1000.0)] == 1.0);

    const auto r = powerAverage (freqs, { ones, scaled (ones, 100.0) }, { ones, w }, false);
    CHECK (r.power[at (40.0)] == doctest::Approx (1.0));
    CHECK (r.power[at (2000.0)] == doctest::Approx (50.5));
}

TEST_CASE ("usable range finds the -10 dB points")
{
    const auto g = logFreqGrid (20.0, 20000.0, 48);
    const auto hp1 = testing::highpass (60.0, 0.5412, 48000.0);   // 4th-order Butterworth = two sections
    const auto hp2 = testing::highpass (60.0, 1.3066, 48000.0);
    std::vector<double> db (g.size());
    for (std::size_t i = 0; i < g.size(); ++i)
        db[i] = hp1.magnitudeDb (g[i], 48000.0) + hp2.magnitudeDb (g[i], 48000.0);
    const auto [lo, hi] = usableRange (g, db);
    CHECK (lo == doctest::Approx (60.0 * 0.76).epsilon (0.03));
    CHECK (hi == doctest::Approx (g.back()));
}

TEST_CASE ("usable range of a sub walks out from its own band")
{
    // Plays 30-100 Hz (4th-order high- and low-pass): nothing at 1 kHz.
    const auto g = logFreqGrid (20.0, 20000.0, 48);
    std::vector<double> db (g.size());
    for (std::size_t i = 0; i < g.size(); ++i)
        db[i] = -10.0 * std::log10 ((1.0 + std::pow (30.0 / g[i], 8.0)) * (1.0 + std::pow (g[i] / 100.0, 8.0)));
    const auto [lo, hi] = usableRange (g, db, 40.0, 100.0);
    CHECK (lo > 20.0);
    CHECK (lo < 30.0);
    CHECK (hi > 100.0);
    CHECK (hi < 150.0);
}

TEST_CASE ("quick mode strength grows with good positions")
{
    CHECK (quickModePolicy (1, 6).smoothingFraction == 2);
    CHECK (quickModePolicy (1, 6).maxCorrectionDb == 3.0);
    CHECK (quickModePolicy (2, 6).smoothingFraction == 3);
    CHECK (quickModePolicy (3, 6).smoothingFraction == 6);
    CHECK_FALSE (quickModePolicy (5, 6).maxCorrectionDb.has_value());
    CHECK (quickModePolicy (1, 6).strength < quickModePolicy (2, 6).strength);
}

TEST_CASE ("session summary excludes and aligns")
{
    const auto p = shape();
    const std::vector<std::shared_ptr<const Capture>> caps { fakeCapture (p), fakeCapture (scaled (p, 4.0)),
                                                             fakeCapture (scaled (p, 100.0), true) };
    const auto s = summarizeSession (caps, 6, logFreqGrid());
    REQUIRE (s.has_value());
    CHECK (s->nGood == 2);
    CHECK (s->policy.smoothingFraction == 3);
    CHECK (s->offsetsDb[0] == doctest::Approx (3.01).epsilon (0.01));
    CHECK (s->offsetsDb[1] == doctest::Approx (-3.01).epsilon (0.01));
    CHECK (s->offsetsDb[2] == doctest::Approx (3.01 - 20.0).epsilon (0.01));

    CHECK_FALSE (summarizeSession ({ fakeCapture (p, true) }, 6, logFreqGrid()).has_value());
}

TEST_CASE ("session summary rebins other sample rates")
{
    const auto f44 = rfftFreqs (32768, 44100.0);
    const std::vector<std::shared_ptr<const Capture>> caps { fakeCapture (ones),
                                                             fakeCapture (std::vector<double> (f44.size(), 2.0), false, 44100.0) };
    const auto s = summarizeSession (caps, 6, logFreqGrid());
    REQUIRE (s.has_value());
    for (std::size_t i = 0; i < s->grid.size(); ++i)
        if (s->grid[i] > 100.0 && s->grid[i] < 10000.0)
            CHECK (s->averageDb[i] == doctest::Approx (10.0 * std::log10 (std::sqrt (2.0))).epsilon (0.01));
}
