#include "roomeq/correction.h"
#include "roomeq/refit.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;
const CorrectionConfig cfg;
const auto x32Octaves = bandwidthForQ (0.3);

Band band (BandKind kind, double freq, double gain, double q = shelfQ)
{
    Band b;
    b.kind = kind;
    b.freq = freq;
    b.gainDb = gain;
    b.q = q;
    return b;
}

// A nine-band correction: a low shelf, a narrow deep cut and broader ones, boosts, a high shelf.
const std::vector<Band> nine {
    band (BandKind::lowShelf, 60, 2.5),      band (BandKind::bell, 95, -6, 4.0),   band (BandKind::bell, 160, -4, 3.0),
    band (BandKind::bell, 240, -3, 2.5),     band (BandKind::bell, 500, 1.5, 1.2), band (BandKind::bell, 1200, -2.5, 2.0),
    band (BandKind::bell, 2500, 2, 1.3),     band (BandKind::bell, 5000, -3, 2.0), band (BandKind::highShelf, 9000, -2),
};

const Refit& fitted (int n, bool shelves)
{
    static std::map<std::pair<int, bool>, Refit> cache;
    const auto key = std::make_pair (n, shelves);
    if (cache.count (key) == 0)
        cache[key] = refitBands (nine, fs, n, shelves, shelves ? cfg.maxOctaves : x32Octaves);
    return cache.at (key);
}
} // namespace

TEST_CASE ("refit: bands that already fit are kept")
{
    const std::vector<Band> five (nine.begin(), nine.begin() + 5);
    const auto r = refitBands (five, fs, 6);
    CHECK_FALSE (r.refitted);
    CHECK (r.bands == five);
    CHECK (r.originalBands == 5);
    CHECK (r.maxErrorDb == 0.0);
    CHECK (r.rmsErrorDb == 0.0);
}

TEST_CASE ("refit: a shelf is refitted when shelves are not allowed")
{
    const std::vector<Band> bands { band (BandKind::lowShelf, 80, 2.0), band (BandKind::bell, 300, -3, 2.0) };
    const auto r = refitBands (bands, fs, 6, false, x32Octaves);
    CHECK (r.refitted);
    REQUIRE_FALSE (r.bands.empty());
    for (const auto& b : r.bands)
        CHECK (b.kind == BandKind::bell);
    CHECK (r.maxErrorDb < 1.0);
}

TEST_CASE ("refit: within the band count, the limits and the widths")
{
    struct Case { int n; bool shelves; double maxErr, rms; };
    const auto grid = refitGrid();
    const auto curve = responseDb (nine, grid, fs);
    for (const auto& c : { Case { 8, true, 0.6, 0.2 }, Case { 6, true, 2.0, 0.7 }, Case { 4, true, 3.5, 1.0 },
                           Case { 6, false, 2.0, 0.7 } })
    {
        CAPTURE (c.n);
        CAPTURE (c.shelves);
        const auto& r = fitted (c.n, c.shelves);
        CHECK (r.refitted);
        CHECK (r.originalBands == 9);
        CHECK (! r.bands.empty());
        CHECK (static_cast<int> (r.bands.size()) <= c.n);
        const auto resp = responseDb (r.bands, grid, fs);
        const auto [maxErr, rms] = fitError (curve, resp);
        CHECK (maxErr == doctest::Approx (r.maxErrorDb));
        CHECK (rms == doctest::Approx (r.rmsErrorDb));
        CHECK (r.maxErrorDb < c.maxErr);
        CHECK (r.rmsErrorDb < c.rms);
        for (std::size_t n = 0; n < grid.size(); ++n)
            CHECK (resp[n] - std::max (curve[n], 0.0) <= refitBoostSlackDb + 0.1);
        const auto widest = c.shelves ? cfg.maxOctaves : x32Octaves;
        for (const auto& b : r.bands)
        {
            CHECK (std::abs (b.gainDb) <= refitGainLimitDb);
            if (! c.shelves)
                CHECK (b.kind == BandKind::bell);
            if (b.kind == BandKind::bell)
            {
                const auto octaves = bandwidthForQ (b.q);
                const auto need = b.gainDb > 0.0 ? cfg.boostMinOctaves : minCutOctaves (b.freq, cfg);
                CHECK (octaves >= need - 1e-6);
                CHECK (octaves <= widest + 1e-6);
            }
        }
    }
}

TEST_CASE ("refit: more bands fit closer, and the narrow deep cut is kept")
{
    CHECK (fitted (8, true).maxErrorDb < fitted (6, true).maxErrorDb);
    CHECK (fitted (6, true).maxErrorDb <= fitted (4, true).maxErrorDb + 1e-9);
    const auto& six = fitted (6, true).bands;
    CHECK (std::any_of (six.begin(), six.end(), [] (const Band& b)
                        { return b.kind == BandKind::bell && b.freq > 85.0 && b.freq < 105.0 && b.gainDb < -4.5; }));
}

TEST_CASE ("refit: no bands leaves the whole curve as error")
{
    const auto r = refitBands (nine, fs, 0);
    CHECK (r.bands.empty());
    const auto curve = responseDb (nine, refitGrid(), fs);
    double largest = 0.0;
    for (auto v : curve)
        largest = std::max (largest, std::abs (v));
    CHECK (r.maxErrorDb == doctest::Approx (largest));
}

TEST_CASE ("refit: fit error")
{
    const auto [largest, rms] = fitError ({ 0.0, 0.05, 2.0, -1.0 }, { 0.0, 0.0, 1.0, -1.5 });
    CHECK (largest == doctest::Approx (1.0));
    CHECK (rms == doctest::Approx (std::sqrt ((1.0 + 0.25) / 2.0)));   // the first two points are flat in both
}
