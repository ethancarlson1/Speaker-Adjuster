#include "helpers.h"
#include "roomeq/capture.h"
#include "roomeq/noise.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace roomeq;

namespace
{
double bandLevelDb (const std::vector<double>& x, double fs, double lo, double hi)
{
    const auto n = nextPow2 (x.size());
    const auto X = rfft (x, n);
    const auto f = rfftFreqs (n, fs);
    double sum = 0.0;
    int count = 0;
    for (std::size_t k = 0; k < X.size(); ++k)
        if (f[k] >= lo && f[k] < hi)
        {
            sum += std::norm (X[k]);
            ++count;
        }
    return 10.0 * std::log10 (sum / count);
}
} // namespace

TEST_CASE ("pink noise: slope, peak level and crest factor")
{
    NoiseConfig cfg;
    cfg.duration = 10.0;
    const auto x = generatePinkNoise (cfg);
    REQUIRE (x.size() == cfg.nNoise());

    double peak = 0.0, sumSq = 0.0;
    for (auto v : x)
    {
        peak = std::max (peak, std::abs (v));
        sumSq += v * v;
    }
    CHECK (peak == doctest::Approx (std::pow (10.0, -12.0 / 20.0)));
    const auto crestDb = 20.0 * std::log10 (peak / std::sqrt (sumSq / static_cast<double> (x.size())));
    CHECK (crestDb > 9.0);
    CHECK (crestDb < 10.5);

    const auto drop = bandLevelDb (x, cfg.fs, 90.0, 110.0) - bandLevelDb (x, cfg.fs, 1440.0, 1760.0);
    CHECK (drop == doctest::Approx (12.0).epsilon (0.125));   // -3 dB/octave over 4 octaves
    CHECK (std::abs (x.front()) < 1e-6);
    CHECK (std::abs (x.back()) < 1e-3);
}

TEST_CASE ("pink noise through a known filter is measured like a sweep")
{
    NoiseConfig cfg;
    cfg.duration = 20.0;
    const auto x = generatePinkNoise (cfg);
    const auto hp = testing::highpass (80.0, 0.7071, cfg.fs);
    const auto peq = testing::peaking (1000.0, -6.0, 1.0, cfg.fs);
    auto played = x;
    played.resize (x.size() + cfg.nTail(), 0.0);
    const auto mic = testing::delayed (peq.process (hp.process (played)), 700);

    const auto c = analyzeProgramCapture ("N", x, mic, cfg.fs);
    CHECK (c.grade.overall == Grade::pass);
    CHECK (c.delaysMs.at (0) == doctest::Approx (1000.0 * 700.0 / cfg.fs).epsilon (0.01));
    for (auto f : testing::geomspace (100.0, 16000.0, 30))
    {
        const auto k = static_cast<std::size_t> (std::lround (f / c.freqs[1]));
        CHECK (std::abs (10.0 * std::log10 (c.power[k]) - (hp.magnitudeDb (f, cfg.fs) + peq.magnitudeDb (f, cfg.fs))) < 0.3);
    }
}
