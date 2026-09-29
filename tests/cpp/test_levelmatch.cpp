#include "helpers.h"
#include "roomeq/levelmatch.h"

#include <doctest/doctest.h>

using namespace roomeq;

namespace
{
// Straightforward reference: K-weighted pink noise power through `bands`, on a fine grid.
double referenceMakeupDb (const std::vector<Band>& bands, double fs)
{
    const auto grid = testing::geomspace (20.0, 20000.0, 961);   // 96 points per octave
    const auto k = kWeightingBands();
    const auto w = responseDb ({ k[0], k[1] }, grid, fs);
    const auto h = responseDb (bands, grid, fs);
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < grid.size(); ++i)
    {
        num += std::pow (10.0, (w[i] + h[i]) / 10.0);
        den += std::pow (10.0, w[i] / 10.0);
    }
    return -10.0 * std::log10 (num / den);
}
} // namespace

TEST_CASE ("level match: nothing to match, broadband gain, clamp")
{
    LevelMatch lm;
    lm.prepare (48000.0);
    CHECK (lm.size() > 200);
    CHECK (lm.makeupDb ({}) == doctest::Approx (0.0));
    // A shelf at 5 Hz is a broadband gain over 20 Hz-20 kHz.
    CHECK (lm.makeupDb ({ { BandKind::highShelf, 5.0, -6.0 } }) == doctest::Approx (6.0).epsilon (0.02));
    CHECK (lm.makeupDb ({ { BandKind::highShelf, 5.0, 4.0 } }) == doctest::Approx (-4.0).epsilon (0.02));
    CHECK (lm.makeupDb ({ { BandKind::highShelf, 5.0, -30.0 } }) == doctest::Approx (LevelMatch::maxGainDb));
    Band off { BandKind::bell, 1000.0, -12.0, 1.0 };
    off.enabled = false;
    CHECK (lm.makeupDb ({ off }) == doctest::Approx (0.0));
}

TEST_CASE ("level match agrees with a fine-grid reference at common sample rates")
{
    const std::vector<Band> bands { { BandKind::bell, 150.0, -8.0, 1.0 },     { BandKind::lowShelf, 120.0, -6.0 },
                                    { BandKind::bell, 2500.0, -4.0, 1.5 },    { BandKind::highShelf, 8000.0, 3.0 },
                                    { BandKind::highPass, 40.0, 0.0, 0.7071 }, { BandKind::bell, 6000.0, -6.0, 6.0 } };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        LevelMatch lm;
        lm.prepare (fs);
        CHECK (lm.makeupDb (bands) == doctest::Approx (referenceMakeupDb (bands, fs)).epsilon (0.03));

        // Stage by stage (as the audio thread does it) is the same as all at once.
        LevelMatch::Power a {}, b {};
        LevelMatch::flat (a);
        LevelMatch::flat (b);
        for (std::size_t i = 0; i < bands.size(); ++i)
            lm.multiply (i < 3 ? a : b, bands[i]);
        CHECK (lm.makeupDb (a, b) == doctest::Approx (lm.makeupDb (bands)).epsilon (1e-9));
    }
}

TEST_CASE ("level match weighs like a loudness meter: treble counts more than deep bass")
{
    LevelMatch lm;
    lm.prepare (48000.0);
    const auto lowCut = lm.makeupDb ({ { BandKind::lowShelf, 200.0, -6.0 } });
    const auto highCut = lm.makeupDb ({ { BandKind::highShelf, 2000.0, -6.0 } });
    const auto subCut = lm.makeupDb ({ { BandKind::lowShelf, 40.0, -12.0 } });
    CHECK (lowCut > 0.3);
    CHECK (highCut > lowCut);
    CHECK (subCut < 0.5);   // K-weighting's high-pass: below ~40 Hz barely counts
}
