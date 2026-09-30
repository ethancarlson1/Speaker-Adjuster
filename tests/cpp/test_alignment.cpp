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

namespace
{
// A Linkwitz-Riley 4th-order crossover half on lfGrid() (two analogue 2nd-order Butterworths), delayed.
LowResponse lr4 (bool highpass, double fc, double delayMs = 0.0, double sign = 1.0)
{
    LowResponse r;
    for (const auto f : lfGrid())
    {
        const cplx s { 0.0, f / fc };
        const auto bw = (highpass ? s * s : cplx { 1.0, 0.0 }) / (s * s + std::sqrt (2.0) * s + 1.0);
        r.h.push_back (sign * bw * bw * std::polar (1.0, -2.0 * testing::pi * f * delayMs * 1e-3));
    }
    r.refMs = 10.0;
    return r;
}
} // namespace

TEST_CASE ("the low-frequency response starts at the loop start, whatever the peak")
{
    // A direct sound at 5 ms and a peak twice as loud 60 ms later: both are in it.
    const double fs = 48000.0;
    std::vector<double> h (48000, 0.0);
    h[1000 + 240] = 1.0;
    h[1000 + 240 + 2880] = 2.0;
    const auto low = lowFrequencyResponse (h, 1000, 1000 + 240 + 2880, fs);
    REQUIRE (low.size() == lfGrid().size());
    for (std::size_t i = 0; i < low.size(); i += 17)
    {
        const auto w = -2.0 * testing::pi * lfGrid()[i] / fs;
        const auto expected = std::polar (1.0, w * 240.0) + std::polar (2.0, w * (240.0 + 2880.0));
        CHECK (std::abs (low[i] - expected) < 1e-9);
    }
}

TEST_CASE ("an LR4 crossover lines up exactly; a reversed sub is inverted; a late sub delays the mains")
{
    const auto out = alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0));
    REQUIRE (out.ok);
    CHECK (out.delayMs == doctest::Approx (3.0).epsilon (0.004));
    CHECK_FALSE (out.invert);
    CHECK (out.efficiencyDb > -0.05);
    CHECK (out.improvementDb > 1.0);
    CHECK (out.crossingHz == doctest::Approx (80.0).epsilon (0.03));
    CHECK ((out.regionLoHz < 80.0 && 80.0 < out.regionHiHz));
    CHECK (out.confidence == Confidence::high);

    SubSettings there;
    there.subDelayMs = 3.0;
    CHECK (alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0), there).improvementDb < 0.01);

    const auto reversed = alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0, 0.0, -1.0));
    CHECK (reversed.invert);
    CHECK (reversed.delayMs == doctest::Approx (3.0).epsilon (0.004));

    const auto late = alignSub (lr4 (true, 80.0), lr4 (false, 80.0, 2.0));
    CHECK (late.delayMs == doctest::Approx (-2.0).epsilon (0.006));
    CHECK (late.note.find ("delay the mains by 2.00 ms") != std::string::npos);

    SubSettings moved;
    moved.mainDelayMs = 1.0;
    moved.mainInvert = true;
    const auto m = alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0), moved);
    CHECK (m.delayMs == doctest::Approx (4.0).epsilon (0.003));
    CHECK (m.invert);
}

TEST_CASE ("no crossover, noise and a narrow overlap")
{
    const auto none = alignSub (lr4 (true, 500.0), lr4 (false, 700.0));
    CHECK_FALSE (none.ok);
    CHECK (none.note.rfind ("No crossover to align", 0) == 0);
    CHECK_FALSE (crossoverRegion (lr4 (true, 500.0), lr4 (false, 700.0)).has_value());
    CHECK (crossoverRegion (lr4 (true, 80.0), lr4 (false, 80.0)).has_value());

    SubSettings noisy;
    noisy.mainSnrDb = 15.0;
    const auto medium = alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0), noisy);
    CHECK (medium.confidence == Confidence::medium);
    REQUIRE_FALSE (medium.reasons.empty());
    CHECK (medium.reasons[0] == "15 dB SNR over the crossover");
    noisy.subSnrDb = 5.0;
    CHECK (alignSub (lr4 (true, 80.0, 3.0), lr4 (false, 80.0), noisy).confidence == Confidence::low);
    CHECK_FALSE (alignSub ({}, lr4 (false, 80.0)).ok);    // pink noise or music: no phase to work with
}
