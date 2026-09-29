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

namespace
{
// The mic's clock running `ppm` fast: y'(n) = y(n / (1 + ppm)) stretches the
// recording, so the delay grows by ppm (to within ppm^2).
std::vector<double> drifted (const std::vector<double>& y, double ppm)
{
    return removeDrift (y, 1e6 * (1.0 / (1.0 + ppm * 1e-6) - 1.0));
}
} // namespace

TEST_CASE ("drift resampler is transparent to 18 kHz")
{
    const auto n = static_cast<std::size_t> (4 * fs);
    const auto rate = 1.0 + 37e-6;
    for (double f : { 100.0, 1000.0, 10000.0, 18000.0 })
    {
        std::vector<double> y (n);
        for (std::size_t i = 0; i < n; ++i)
            y[i] = std::sin (2.0 * pi * f * static_cast<double> (i) / fs);
        const auto out = removeDrift (y, 37.0);
        double worst = 0.0;
        for (auto i = static_cast<std::size_t> (fs); i < 3 * static_cast<std::size_t> (fs); ++i)
            worst = std::max (worst, std::abs (out[i] - std::sin (2.0 * pi * f * static_cast<double> (i) * rate / fs)));
        CHECK (20.0 * std::log10 (worst) < -70.0);
    }
}

TEST_CASE ("clock drift is measured, left alone when there is none, and a jumping delay is called unsteady")
{
    const auto x = whiteNoise (static_cast<std::size_t> (20 * fs), 0.1, 5);
    const auto y = system (x, 2400);
    for (double ppm : { -40.0, -8.0, 3.0, 12.0, 40.0 })
    {
        const auto d = estimateDrift (x, drifted (y, ppm), fs);
        CHECK (d.status == DriftEstimate::Status::corrected);
        CHECK (std::abs (d.ppm - ppm) < 0.1);
    }
    const auto none = estimateDrift (x, y, fs);
    CHECK (none.status == DriftEstimate::Status::none);
    CHECK (none.ppm == 0.0);
    CHECK (std::abs (none.measuredPpm) < 0.05);
    CHECK (driftNotes (none).empty());

    // An aggregate device glitching: the delay jumps by 5 ms twice.
    const auto third = y.size() / 3, jump = static_cast<std::size_t> (0.005 * fs);
    std::vector<double> jumpy (y.begin(), y.begin() + static_cast<long> (third));
    jumpy.insert (jumpy.end(), jump, 0.0);
    jumpy.insert (jumpy.end(), y.begin() + static_cast<long> (third), y.begin() + static_cast<long> (2 * third));
    jumpy.insert (jumpy.end(), jump, 0.0);
    jumpy.insert (jumpy.end(), y.begin() + static_cast<long> (2 * third), y.end());
    jumpy.resize (x.size());
    const auto bad = estimateDrift (x, jumpy, fs);
    CHECK (bad.status == DriftEstimate::Status::unsteady);
    CHECK (driftNotes (bad) == std::vector<std::string> { "the delay between the output and the mic jumps around: use one audio interface for both" });
}

TEST_CASE ("across two clocks the transfer function matches the single-clock one")
{
    const auto x = whiteNoise (static_cast<std::size_t> (20 * fs), 0.1, 6);
    const auto y = system (x, 2400);
    const auto ref = transferFunction (x, y, fs);
    const auto est = transferFunction (x, drifted (y, 25.0), fs);
    CHECK (est.drift.status == DriftEstimate::Status::corrected);

    // Without the correction, the same capture loses its top end (the aggregate-device failure).
    DualFFTConfig off;
    off.driftMinPpm = 1e9;
    const auto raw = transferFunction (x, drifted (y, 25.0), fs, off);
    double rawCoh = 0.0;
    int n = 0;
    for (std::size_t k = 0; k < raw.freqs.size(); ++k)
        if (raw.freqs[k] > 3000.0 && raw.freqs[k] < 7000.0)
        {
            rawCoh += raw.coherence[k];
            ++n;
        }
    CHECK (rawCoh / n < 0.3);
    CHECK (driftNotes (est.drift).front().rfind ("output and mic clocks differ by 25.0 ppm", 0) == 0);
    CHECK (est.delay == ref.delay);
    for (std::size_t k = 0; k < est.freqs.size(); ++k)
        if (est.freqs[k] > 200.0 && est.freqs[k] < 7000.0)
        {
            CHECK (est.coherence[k] > 0.97);
            CHECK (std::abs (20.0 * std::log10 (std::abs (est.H[k]) / std::abs (ref.H[k]))) < 0.1);
        }
}
