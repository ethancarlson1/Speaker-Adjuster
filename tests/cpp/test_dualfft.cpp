#include "helpers.h"
#include "roomeq/dualfft.h"

#include <doctest/doctest.h>

using namespace roomeq;
using namespace testing;

namespace
{
constexpr double fs = 48000.0;
const auto hp = highpass (100.0, 0.7071, fs);
const auto lp = lowpass (8000.0, 0.7071, fs);

std::vector<double> system (const std::vector<double>& x, std::size_t delay)
{
    return delayed (lp.process (hp.process (x)), delay);
}
} // namespace

TEST_CASE ("delay and magnitude with a clean input")
{
    const auto x = whiteNoise (static_cast<std::size_t> (10 * fs), 1.0, 1);
    const auto est = transferFunction (x, system (x, 300), fs, {}, 8192);
    CHECK (est.delay >= 300);
    CHECK (est.delay <= 302);   // plus the filters' own peak delay
    for (std::size_t k = 0; k < est.freqs.size(); ++k)
    {
        const auto f = est.freqs[k];
        if (f > 200.0 && f < 6000.0)
        {
            CHECK (std::abs (20.0 * std::log10 (std::abs (est.H[k])) - (hp.magnitudeDb (f, fs) + lp.magnitudeDb (f, fs))) < 0.2);
            CHECK (est.coherence[k] > 0.98);
        }
    }
}

TEST_CASE ("mic noise lowers coherence but does not bias H1")
{
    const auto x = whiteNoise (static_cast<std::size_t> (20 * fs), 1.0, 2);
    auto y = system (x, 300);
    double var = 0.0;
    for (auto v : y)
        var += v * v;
    const auto noise = whiteNoise (y.size(), std::sqrt (var / static_cast<double> (y.size())), 3);
    for (std::size_t i = 0; i < y.size(); ++i)
        y[i] += noise[i];   // 0 dB SNR at the mic

    const auto est = transferFunction (x, y, fs, {}, 8192);
    double sumH = 0.0, sumTrue = 0.0, sumCoh = 0.0;
    int n = 0;
    for (std::size_t k = 0; k < est.freqs.size(); ++k)
    {
        const auto f = est.freqs[k];
        if (f > 500.0 && f < 4000.0)
        {
            sumH += std::norm (est.H[k]);
            sumTrue += std::pow (10.0, (hp.magnitudeDb (f, fs) + lp.magnitudeDb (f, fs)) / 10.0);
            sumCoh += est.coherence[k];
            ++n;
        }
    }
    CHECK (sumCoh / n < 0.8);
    CHECK (std::abs (10.0 * std::log10 (sumH / sumTrue)) < 0.3);
}

TEST_CASE ("excitation gate blanks gaps between harmonics")
{
    const auto n = static_cast<std::size_t> (10 * fs);
    auto x = whiteNoise (n, 1e-4, 4);
    for (int h = 1; h <= 20; ++h)
        for (std::size_t i = 0; i < n; ++i)
            x[i] += std::sin (2.0 * pi * 200.0 * h * static_cast<double> (i) / fs + h);
    const auto est = transferFunction (x, system (x, 0), fs, {}, 8192);
    const auto gate = excitationGate (est);
    const auto df = est.freqs[1];
    for (int h = 5; h < 20; ++h)
    {
        CHECK (gate[static_cast<std::size_t> (std::lround (200.0 * h / df))] == 1.0);
        CHECK (gate[static_cast<std::size_t> (std::lround ((200.0 * h + 100.0) / df))] == 0.0);
    }
}
