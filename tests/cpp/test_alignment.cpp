#include "helpers.h"
#include "roomeq/alignment.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace roomeq;

namespace
{
double besselI0 (double x)   // std::cyl_bessel_i isn't in every standard library (libc++)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

// A band-limited impulse at a fractional delay (windowed sinc), as the prototype's tests use.
std::vector<double> fractionalImpulse (double delay, std::size_t n)
{
    std::vector<double> h (n, 0.0);
    constexpr int taps = 64;
    const auto k0 = static_cast<long> (std::floor (delay));
    for (long k = k0 - taps; k <= k0 + taps; ++k)
    {
        const auto x = static_cast<double> (k) - delay;
        const auto r = static_cast<double> (k - (k0 - taps)) / (2.0 * taps) * 2.0 - 1.0;   // -1..1 across the taps
        const auto kaiser = besselI0 (8.0 * std::sqrt (std::max (0.0, 1.0 - r * r))) / besselI0 (8.0);
        const auto sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (testing::pi * x) / (testing::pi * x);
        h[static_cast<std::size_t> (k)] = sinc * kaiser;
    }
    return h;
}

Capture sweepThrough (const std::vector<std::vector<double>>& irs)
{
    SweepConfig cfg;
    cfg.duration = 2.0;
    const auto play = buildPlayback (cfg, generateSweep (cfg));
    std::vector<std::vector<double>> recs;
    unsigned seed = 1;
    for (const auto& ir : irs)
    {
        auto y = testing::fftConvolve (play, ir);
        y.resize (play.size());
        const auto noise = testing::whiteNoise (y.size(), 1e-5, seed++);
        for (std::size_t i = 0; i < y.size(); ++i)
            y[i] += noise[i];
        recs.push_back (y);
    }
    return analyzeSweepCapture ("P", recs, cfg);
}

std::vector<double> plus (std::vector<double> a, const std::vector<double>& b, double gain)
{
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] += gain * b[i];
    return a;
}
} // namespace

TEST_CASE ("the direct arrival is found below a sample, and a later reflection doesn't move it")
{
    const auto ir = plus (fractionalImpulse (480.37, 4096), fractionalImpulse (600.0, 4096), 0.9);   // a floor bounce 2.5 ms later
    const auto a = estimateArrival (sweepThrough ({ ir, ir }));
    CHECK (a.ms == doctest::Approx (1000.0 * 480.37 / 48000.0).epsilon (0.0004));
    CHECK (a.confidence == Confidence::high);
}

TEST_CASE ("a blocked direct sound and moving repeats lower the confidence")
{
    const auto blocked = plus (fractionalImpulse (624.0, 4096), fractionalImpulse (480.0, 4096), 0.35);
    const auto a = estimateArrival (sweepThrough ({ blocked }));
    CHECK (a.confidence == Confidence::medium);
    REQUIRE_FALSE (a.reasons.empty());
    CHECK (a.reasons[0].rfind ("an earlier arrival 9.1 dB down, 3.0 ms before", 0) == 0);

    const auto moved = estimateArrival (sweepThrough ({ fractionalImpulse (480.0, 4096), fractionalImpulse (500.0, 4096) }));
    CHECK (moved.confidence == Confidence::low);
}

TEST_CASE ("distance and the zone delay suggestion")
{
    CHECK (equivalentDistanceM (12.5) == doctest::Approx (4.2875));
    CHECK (equivalentDistanceM (18.5, 6.0) == doctest::Approx (4.2875));
    CHECK (suggestZoneDelay (23.40, 8.20).delayMs == doctest::Approx (15.20));
    CHECK (suggestZoneDelay (23.40, 8.20, 2.0).delayMs == doctest::Approx (17.20));
    const auto late = suggestZoneDelay (8.0, 10.5);
    CHECK (late.delayMs == 0.0);
    CHECK (late.differenceMs == doctest::Approx (-2.5));
    CHECK (late.note.find ("already arrives 2.50 ms after") != std::string::npos);
}
