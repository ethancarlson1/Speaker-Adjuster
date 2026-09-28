#include "helpers.h"
#include "roomeq/spectrum.h"
#include "roomeq/sweep.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace roomeq;
using namespace testing;

TEST_CASE ("sweep follows the exponential frequency law")
{
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto x = generateSweep (cfg);
    for (auto frac : { 0.25, 0.5, 0.75 })
    {
        // Count zero crossings in a 50 ms window: f ~ crossings / (2 * window).
        const auto centre = static_cast<std::size_t> (frac * static_cast<double> (x.size()));
        const auto half = static_cast<std::size_t> (0.025 * cfg.fs);
        int crossings = 0;
        for (auto i = centre - half + 1; i < centre + half; ++i)
            crossings += (x[i - 1] < 0.0) != (x[i] < 0.0);
        const auto measured = crossings / (2.0 * 0.05);
        const auto expected = cfg.f1 * std::exp (static_cast<double> (centre) / cfg.fs / cfg.rate());
        CHECK (measured == doctest::Approx (expected).epsilon (0.03));
    }
}

TEST_CASE ("sweep level and fades")
{
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto x = generateSweep (cfg);
    double peak = 0.0;
    for (auto v : x)
        peak = std::max (peak, std::abs (v));
    CHECK (peak == doctest::Approx (std::pow (10.0, -12.0 / 20.0)).epsilon (1e-3));
    CHECK (std::abs (x.front()) < 1e-6);
    CHECK (std::abs (x.back()) < 1e-3);
}

TEST_CASE ("identity loop deconvolves to a flat unit impulse")
{
    for (auto [fs, duration] : { std::pair { 44100.0, 2.0 }, { 48000.0, 5.0 }, { 96000.0, 2.0 } })
    {
        SweepConfig cfg;
        cfg.fs = fs;
        cfg.duration = duration;
        const auto x = generateSweep (cfg);
        const auto dec = deconvolve (buildPlayback (cfg, x), x, fs, cfg.f1, cfg.f2);
        const auto arrival = findArrival (dec, cfg.nPreroll());
        CHECK (arrival == dec.zero + cfg.nPreroll());

        const auto n = static_cast<std::size_t> (fs);
        const std::vector<double> seg (dec.h.begin() + static_cast<std::ptrdiff_t> (arrival - n),
                                       dec.h.begin() + static_cast<std::ptrdiff_t> (arrival + n));
        for (auto f : geomspace (cfg.f1, 19500.0, 60))
            CHECK (std::abs (dtftDb (seg, f, fs)) < 0.01);
    }
}

TEST_CASE ("recovers a known filter and the loop delay")
{
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto fs = cfg.fs;
    const auto hp = highpass (80.0, 0.7071, fs);
    const auto peq = peaking (1000.0, -6.0, 1.0, fs);
    const std::size_t delay = 1234;

    const auto x = generateSweep (cfg);
    const auto rec = delayed (peq.process (hp.process (buildPlayback (cfg, x))), delay);
    const auto dec = deconvolve (rec, x, fs, cfg.f1, cfg.f2);

    std::vector<double> imp (4096, 0.0);
    imp[0] = 1.0;
    const auto trueIr = peq.process (hp.process (imp));
    const auto trueIrPeak = static_cast<std::size_t> (std::max_element (trueIr.begin(), trueIr.end(),
                                                                        [] (double a, double b) { return std::abs (a) < std::abs (b); })
                                                      - trueIr.begin());
    CHECK (findArrival (dec, cfg.nPreroll()) - dec.zero - cfg.nPreroll() == delay + trueIrPeak);

    const auto start = dec.zero + cfg.nPreroll() + delay;
    const std::vector<double> seg (dec.h.begin() + static_cast<std::ptrdiff_t> (start - 9600),
                                   dec.h.begin() + static_cast<std::ptrdiff_t> (start + 24000));
    for (auto f : geomspace (40.0, 18000.0, 60))
        CHECK (std::abs (dtftDb (seg, f, fs) - (hp.magnitudeDb (f, fs) + peq.magnitudeDb (f, fs))) < 0.1);
}

TEST_CASE ("fractional peak offset finds sub-sample delays")
{
    auto a = whiteNoise (4096, 1.0, 1);
    a = lowpass (7000.0, 0.7071, 48000.0).process (lowpass (7000.0, 0.7071, 48000.0).process (a));
    const auto A = rfft (a, a.size());
    const auto freqs = rfftFreqs (a.size(), 1.0);
    for (auto shift : { 0.3, -0.45, 2.25 })
    {
        auto B = A;
        for (std::size_t k = 0; k < B.size(); ++k)
            B[k] *= std::polar (1.0, -2.0 * pi * freqs[k] * shift);
        const auto b = irfft (B, a.size());
        CHECK (std::abs (fractionalPeakOffset (a, b, 8) - shift) < 0.05);
    }
}
