#include "helpers.h"
#include "roomeq/spectrum.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>

using namespace roomeq;

namespace
{
const auto freqs = rfftFreqs (32768, 48000.0);
const auto grid = logFreqGrid (20.0, 20000.0, 48);
} // namespace

TEST_CASE ("log grid spacing")
{
    CHECK (grid.front() == 20.0);
    CHECK (grid.back() <= 20000.0);
    for (std::size_t i = 1; i < grid.size(); ++i)
        CHECK (std::log2 (grid[i] / grid[i - 1]) == doctest::Approx (1.0 / 48.0));
}

TEST_CASE ("smoothing keeps a flat spectrum flat")
{
    const std::vector<double> flat (freqs.size(), 2.0);
    for (auto fraction : { 3.0, 6.0, 24.0 })
        for (auto v : smoothPower (freqs, flat, fraction, grid))
            CHECK (v == doctest::Approx (2.0));
}

TEST_CASE ("smoothing bandwidth matches the fraction")
{
    std::vector<double> p (freqs.size(), 0.0);
    p[static_cast<std::size_t> (std::lround (4000.0 / freqs[1]))] = 1.0;
    const auto fine = logFreqGrid (2000.0, 8000.0, 480);
    const auto out = smoothPower (freqs, p, 3.0, fine);
    const auto nonzero = std::count_if (out.begin(), out.end(), [] (double v) { return v > 0.0; });
    CHECK (static_cast<double> (nonzero) / 480.0 == doctest::Approx (1.0 / 3.0).epsilon (0.03));
}

TEST_CASE ("wider smoothing fills a notch more")
{
    std::vector<double> p (freqs.size(), 1.0);
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (freqs[k] > 980.0 && freqs[k] < 1020.0)
            p[k] = 1e-4;
    const std::vector<double> at { 1000.0 };
    const auto d3 = toDb (smoothPower (freqs, p, 3.0, at)[0]);
    const auto d6 = toDb (smoothPower (freqs, p, 6.0, at)[0]);
    const auto d24 = toDb (smoothPower (freqs, p, 24.0, at)[0]);
    CHECK (d3 > d6);
    CHECK (d6 > d24);
    CHECK (d3 > -1.0);
}

TEST_CASE ("zero-weight bins are ignored, even when NaN")
{
    std::vector<double> p (freqs.size(), 1.0), w (freqs.size(), 1.0);
    for (std::size_t k = 0; k < freqs.size(); ++k)
    {
        if (freqs[k] < 20.0)
        {
            p[k] = std::numeric_limits<double>::quiet_NaN();
            w[k] = 0.0;
        }
        else if (freqs[k] > 1000.0 && freqs[k] < 1100.0)
        {
            p[k] = 100.0;
            w[k] = 0.0;
        }
    }
    for (auto v : smoothPower (freqs, p, 3.0, grid, &w))
        CHECK (v == doctest::Approx (1.0));
}

TEST_CASE ("rebin preserves level")
{
    const auto fine = rfftFreqs (65536, 48000.0);
    std::vector<double> p (fine.size());
    for (std::size_t k = 0; k < fine.size(); ++k)
        p[k] = 1.0 + 0.5 * std::sin (fine[k] / 300.0);
    const auto out = rebinPower (fine, p, freqs);
    for (std::size_t k = 1; k + 1 < freqs.size(); ++k)
        CHECK (std::abs (out[k] - (1.0 + 0.5 * std::sin (freqs[k] / 300.0))) < 1e-3);
}

TEST_CASE ("analysis window shape")
{
    const auto win = irWindow (WindowConfig {}, 48000.0);
    CHECK (win.w.size() == 26400);
    CHECK (win.nPre == 2400);
    CHECK (win.w.front() == 0.0);
    CHECK (win.w[1200] == 1.0);
    CHECK (win.w[2400 + 14400] == 1.0);
    CHECK (win.w.back() < 1e-3);
    CHECK (responseNfft (WindowConfig {}, 48000.0) == 32768);
}

TEST_CASE ("formatHz uses two significant figures")
{
    CHECK (formatHz (88.4) == "88 Hz");
    CHECK (formatHz (176.8) == "180 Hz");
    CHECK (formatHz (1414.0) == "1.4 kHz");
    CHECK (formatHz (11314.0) == "11 kHz");
    CHECK (formatHz (22.1) == "22 Hz");
    CHECK (formatHz (99.6) == "100 Hz");
}
