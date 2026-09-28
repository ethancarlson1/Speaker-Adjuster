#include "helpers.h"
#include "roomeq/filters.h"
#include "roomeq/spectrum.h"
#include "roomeq/targets.h"

#include <doctest/doctest.h>

using namespace roomeq;

namespace
{
double at (const Band& b, double f, double fs = 48000.0)
{
    return bandDb (b, { f }, fs).front();
}
} // namespace

TEST_CASE ("biquads match the reference RBJ designs")
{
    const auto freqs = testing::geomspace (20.0, 20000.0, 200);
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto hp = testing::highpass (80.0, 0.7071, fs);
        const auto pk = testing::peaking (1000.0, -6.0, 2.0, fs);
        const auto ours = responseDb ({ { BandKind::highPass, 80.0, 0.0, 0.7071 }, { BandKind::bell, 1000.0, -6.0, 2.0 } },
                                      freqs, fs);
        for (std::size_t i = 0; i < freqs.size(); ++i)
            CHECK (ours[i] == doctest::Approx (hp.magnitudeDb (freqs[i], fs) + pk.magnitudeDb (freqs[i], fs)).epsilon (1e-9));
    }
}

TEST_CASE ("band gains at their defining frequencies")
{
    CHECK (at ({ BandKind::bell, 1000.0, -6.0, 2.0 }, 1000.0) == doctest::Approx (-6.0));
    CHECK (at ({ BandKind::lowShelf, 100.0, 4.0 }, 100.0) == doctest::Approx (2.0));
    CHECK (at ({ BandKind::lowShelf, 100.0, 4.0 }, 20.0) == doctest::Approx (4.0).epsilon (0.05));
    CHECK (at ({ BandKind::highShelf, 8000.0, -3.0 }, 8000.0) == doctest::Approx (-1.5));
    CHECK (at ({ BandKind::highPass, 80.0, 0.0, 0.7071 }, 80.0) == doctest::Approx (-3.01).epsilon (0.01));
    CHECK (at ({ BandKind::lowPass, 12000.0, 0.0, 0.7071 }, 12000.0) == doctest::Approx (-3.01).epsilon (0.01));
    Band off { BandKind::bell, 1000.0, -6.0, 2.0, false };
    CHECK (at (off, 1000.0) == 0.0);
}

TEST_CASE ("bandwidth and Q round trip")
{
    CHECK (qForBandwidth (1.0) == doctest::Approx (std::sqrt (2.0)));
    for (double octaves : { 1.0 / 3.0, 2.0 / 3.0, 1.0, 3.0 })
        CHECK (bandwidthForQ (qForBandwidth (octaves)) == doctest::Approx (octaves));
}

TEST_CASE ("target presets hit the agreed numbers")
{
    const auto& house = houseTarget();
    CHECK (house.db (40.0) == doctest::Approx (4.0));
    CHECK (house.db (250.0) == 0.0);
    CHECK (house.db (1000.0) == 0.0);
    CHECK (house.db (16000.0) == doctest::Approx (-3.0));
    CHECK (house.db (10.0) == 4.0);            // held beyond the end points
    const auto& speech = speechTarget();
    CHECK (speech.db (75.0) == doctest::Approx (-6.0));
    CHECK (speech.db (2800.0) == doctest::Approx (2.0));
    CHECK (speech.db (16000.0) == doctest::Approx (-3.0));
    CHECK (flatTarget().db (123.0) == 0.0);
    CHECK (targetPresets().size() == 3);

    // Shape-preserving: monotone between monotone points (no overshoot).
    const auto f = testing::geomspace (50.0, 250.0, 100);
    const auto h = house.db (f);
    for (std::size_t i = 1; i < h.size(); ++i)
        CHECK (h[i] <= h[i - 1] + 1e-12);
}

TEST_CASE ("pchip matches scipy on a known curve")
{
    // scipy.interpolate.PchipInterpolator(log2([20, 100, 1000, 10000]), [0, 6, -2, -1])(log2(f))
    const std::vector<std::pair<double, double>> pts { { 20.0, 0.0 }, { 100.0, 6.0 }, { 1000.0, -2.0 }, { 10000.0, -1.0 } };
    const auto y = pchipLog (pts, { 30.0, 300.0, 3000.0 });
    CHECK (y[0] == doctest::Approx (2.468787432746).epsilon (1e-8));
    CHECK (y[1] == doctest::Approx (2.274353334061).epsilon (1e-8));
    CHECK (y[2] == doctest::Approx (-1.891385879063).epsilon (1e-8));
}

TEST_CASE ("anchor offset and point sanitising")
{
    const auto grid = logFreqGrid (20.0, 20000.0, 24);
    auto level = houseTarget().db (grid);
    for (auto& v : level)
        v -= 20.0;
    CHECK (anchorOffsetDb (grid, level, houseTarget()) == doctest::Approx (-20.0));

    const auto pts = sanitizeTargetPoints ({ { 5000.0, 1.0 }, { 10.0, 40.0 }, { 5001.0, 2.0 }, { 100.0, -3.0 } });
    REQUIRE (pts.size() == 3);
    CHECK (pts[0] == std::pair<double, double> { 20.0, 24.0 });
    CHECK (pts[1].first == 100.0);
    CHECK (pts[2] == std::pair<double, double> { 5000.0, 1.0 });
}
