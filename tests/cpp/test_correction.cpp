#include "helpers.h"
#include "roomeq/correction.h"
#include "roomeq/spectrum.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;
const CorrectionConfig cfg;
const auto grid = logFreqGrid (20.0, 20000.0, cfg.pointsPerOctave);

bool inRange (double f) { return f >= 30.0 && f <= 18000.0; }

FitProblem problem (const std::vector<double>& desired, const std::vector<double>* upper = nullptr)
{
    FitProblem p;
    p.freqs = grid;
    p.fs = fs;
    p.fLo = 30.0;
    p.fHi = 18000.0;
    p.cfg = cfg;
    for (std::size_t i = 0; i < grid.size(); ++i)
    {
        const auto inside = inRange (grid[i]);
        p.upper.push_back (upper != nullptr ? (*upper)[i] : (inside ? 3.0 : 0.0));
        p.lower.push_back (-12.0);
        p.desired.push_back (inside ? std::clamp (desired[i], p.lower.back(), p.upper.back()) : 0.0);
        p.weight.push_back (inside ? 1.0 : cfg.outsideWeight);
    }
    return p;
}

void checkWidths (const std::vector<Band>& bands)
{
    for (const auto& b : bands)
    {
        if (b.kind != BandKind::bell)
            continue;
        const auto octaves = bandwidthForQ (b.q);
        const auto need = b.gainDb > 0.0 ? cfg.boostMinOctaves : minCutOctaves (b.freq, cfg);
        CHECK (octaves >= need - 1e-6);
        CHECK (octaves <= cfg.maxOctaves + 1e-6);
    }
}
} // namespace

TEST_CASE ("fit recovers a reachable correction")
{
    const std::vector<Band> truth { { BandKind::bell, 150.0, -6.0, 1.0 }, { BandKind::bell, 3000.0, 2.0, 1.2 },
                                    { BandKind::highShelf, 8000.0, -3.0 } };
    const auto prob = problem (responseDb (truth, grid, fs));
    const auto bands = fitBands (prob);
    const auto fit = responseDb (bands, grid, fs);
    double sumSq = 0.0;
    int n = 0;
    for (std::size_t i = 0; i < grid.size(); ++i)
        if (inRange (grid[i]))
        {
            sumSq += (fit[i] - prob.desired[i]) * (fit[i] - prob.desired[i]);
            ++n;
        }
    CHECK (std::sqrt (sumSq / n) < 0.25);
    CHECK (bands.size() <= 5);
    CHECK (std::is_sorted (bands.begin(), bands.end(), [] (const Band& a, const Band& b) { return a.freq < b.freq; }));
}

TEST_CASE ("fit keeps its limits and widths on a hostile curve")
{
    std::mt19937 rng (5);
    std::normal_distribution<double> dist (0.0, 1.0);
    auto desired = responseDb ({ { BandKind::bell, 2000.0, 10.0, 8.0 }, { BandKind::bell, 100.0, -20.0, 0.8 } }, grid, fs);
    for (std::size_t i = 0; i < desired.size(); ++i)
        desired[i] += 1.5 * std::sin (static_cast<double> (i) * 1.7) + 0.5 * dist (rng);
    const auto bands = fitBands (problem (desired));
    const auto fit = responseDb (bands, grid, fs);
    CHECK (bands.size() <= static_cast<std::size_t> (cfg.maxBands));
    CHECK (*std::max_element (fit.begin(), fit.end()) <= 3.0 + 0.2);
    CHECK (*std::min_element (fit.begin(), fit.end()) >= -12.0 - 0.2);
    checkWidths (bands);
}

TEST_CASE ("fit never boosts into a null")
{
    std::vector<double> upper (grid.size());
    std::vector<bool> null (grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i)
    {
        null[i] = grid[i] > 800.0 && grid[i] < 1300.0;
        upper[i] = inRange (grid[i]) && ! null[i] ? 3.0 : 0.0;
    }
    const auto desired = bandDb ({ BandKind::bell, 1000.0, 6.0, 0.7 }, grid, fs);
    const auto fit = responseDb (fitBands (problem (desired, &upper)), grid, fs);
    double inNull = -100.0, peak = -100.0;
    for (std::size_t i = 0; i < grid.size(); ++i)
    {
        peak = std::max (peak, fit[i]);
        if (null[i])
            inNull = std::max (inNull, fit[i]);
    }
    CHECK (inNull <= 0.25);
    CHECK (peak > 1.0);   // still boosts either side of it
}

TEST_CASE ("nulls from dips and from positions disagreeing, widened by the margin")
{
    std::vector<double> trend (grid.size(), 0.0), fine (grid.size(), 0.0);
    std::vector<std::vector<double>> positions (2, std::vector<double> (grid.size(), 0.0));
    for (std::size_t i = 0; i < grid.size(); ++i)
    {
        if (grid[i] > 950.0 && grid[i] < 1050.0)
            fine[i] = -9.0;
        if (grid[i] > 60.0 && grid[i] < 80.0)
            positions[1][i] = 8.0;
    }
    const auto null = detectNulls (grid, fine, trend, positions, cfg);
    const auto nearest = [] (double f)
    {
        std::size_t k = 0;
        for (std::size_t i = 1; i < grid.size(); ++i)
            if (std::abs (grid[i] - f) < std::abs (grid[k] - f))
                k = i;
        return k;
    };
    CHECK (null[nearest (1000.0)]);
    CHECK (null[nearest (70.0)]);
    CHECK (null[nearest (1000.0 * std::exp2 (1.0 / 8.0))]);
    CHECK_FALSE (null[nearest (300.0)]);
    CHECK_FALSE (null[nearest (5000.0)]);
}

TEST_CASE ("an empty fit range fits nothing")
{
    auto prob = problem (std::vector<double> (grid.size(), -5.0));
    prob.fLo = 5000.0;
    prob.fHi = 4000.0;
    CHECK (fitBands (prob).empty());
}
