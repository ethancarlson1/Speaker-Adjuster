#include "roomeq/refit.h"
#include "roomeq/x32.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <sstream>

using namespace roomeq;

namespace
{
constexpr double fs = 48000.0;

Band band (BandKind kind, double freq, double gain, double q = shelfQ)
{
    Band b;
    b.kind = kind;
    b.freq = freq;
    b.gainDb = gain;
    b.q = q;
    return b;
}

const std::vector<Band> nine {
    band (BandKind::lowShelf, 60, 2.5),      band (BandKind::bell, 95, -6, 4.0),   band (BandKind::bell, 160, -4, 3.0),
    band (BandKind::bell, 240, -3, 2.5),     band (BandKind::bell, 500, 1.5, 1.2), band (BandKind::bell, 1200, -2.5, 2.0),
    band (BandKind::bell, 2500, 2, 1.3),     band (BandKind::bell, 5000, -3, 2.0), band (BandKind::highShelf, 9000, -2),
};

std::vector<std::string> lines (const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in (text);
    for (std::string line; std::getline (in, line);)
        out.push_back (line);
    return out;
}

void checkOnSteps (const std::vector<Band>& bands)
{
    for (const auto& b : bands)
    {
        CHECK (b.kind == BandKind::bell);
        CHECK (x32::snapFrequency (b.freq) == doctest::Approx (b.freq).epsilon (1e-12));
        CHECK (x32::snapGain (b.gainDb) == doctest::Approx (b.gainDb).epsilon (1e-12));
        CHECK (x32::snapQ (b.q) == doctest::Approx (b.q).epsilon (1e-12));
        CHECK (b.gainDb != 0.0);
    }
}
} // namespace

TEST_CASE ("x32: frequencies on the desk's 201 steps, written as it writes them")
{
    // Values the desk was seen to snap to (a band at 200 Hz came back 202.3; 3k00 came back 2k99).
    CHECK (x32::frequencyToken (x32::snapFrequency (200.0)) == "202.3");
    CHECK (x32::frequencyToken (x32::snapFrequency (3000.0)) == "2k99");
    CHECK (x32::frequencyToken (x32::snapFrequency (20.0)) == "20.0");
    CHECK (x32::frequencyToken (x32::snapFrequency (20000.0)) == "20k00");
    CHECK (x32::frequencyToken (x32::snapFrequency (5.0)) == "20.0");
    CHECK (x32::frequencyToken (x32::snapFrequency (30000.0)) == "20k00");
    CHECK (x32::frequencyToken (85.3) == "85.3");
    CHECK (x32::frequencyToken (4529.3) == "4k53");
    CHECK (x32::frequencyToken (10020.0) == "10k02");
    CHECK (x32::frequencyToken (999.96) == "1k00");
    // Each step's token reads back as that step.
    for (int i = 0; i < x32::frequencySteps; ++i)
    {
        const auto f = 20.0 * std::pow (10.0, 3.0 * i / 200.0);
        auto token = x32::frequencyToken (f);
        if (const auto k = token.find ('k'); k != std::string::npos)
            token = token.substr (0, k) + "." + token.substr (k + 1);
        const auto value = std::strtod (token.c_str(), nullptr) * (f >= 999.95 ? 1000.0 : 1.0);
        CHECK (x32::snapFrequency (value) == doctest::Approx (f).epsilon (1e-12));
    }
}

TEST_CASE ("x32: gain in 0.25 dB steps within 15 dB")
{
    CHECK (x32::snapGain (3.1) == 3.0);
    CHECK (x32::snapGain (3.13) == 3.25);
    CHECK (x32::snapGain (-4.8) == -4.75);
    CHECK (x32::snapGain (20.0) == 15.0);
    CHECK (x32::snapGain (-20.0) == -15.0);
    CHECK (x32::gainToken (4.75) == "+4.75");
    CHECK (x32::gainToken (-0.25) == "-0.25");
    CHECK (x32::gainToken (0.0) == "+0.00");
    CHECK (x32::gainToken (-0.0) == "+0.00");
    CHECK (x32::gainToken (-12.0) == "-12.0");
    CHECK (x32::gainToken (10.75) == "+10.8");
    CHECK (x32::snapGain (10.8) == 10.75);   // the desk reads "+10.8" back as 10.75
}

TEST_CASE ("x32: Q on the desk's 72 steps that a one-decimal value reaches")
{
    CHECK (x32::qToken (10.0) == "10");
    CHECK (x32::qToken (9.97) == "10");
    CHECK (x32::qToken (0.3) == "0.3");
    CHECK (x32::qToken (2.0) == "2.0");
    CHECK (x32::snapQ (100.0) == doctest::Approx (10.0));
    CHECK (x32::snapQ (0.01) == doctest::Approx (0.3));
    // Steps seen in a desk's file: 1.6, 3.9 and 2.0.
    CHECK (x32::qToken (x32::snapQ (1.6)) == "1.6");
    CHECK (x32::qToken (x32::snapQ (3.9)) == "3.9");
    CHECK (x32::qToken (x32::snapQ (2.0)) == "2.0");
    // Whatever Q is asked for, its token reads back as the same step.
    for (double q = 0.25; q < 12.0; q *= 1.01)
    {
        const auto v = x32::snapQ (q);
        CHECK (x32::snapQ (std::strtod (x32::qToken (v).c_str(), nullptr)) == doctest::Approx (v).epsilon (1e-12));
    }
}

TEST_CASE ("x32: the correction refitted to six bells on the desk's steps")
{
    const auto fit = x32::fitForDesk (nine, fs);
    CHECK (fit.refitted);
    CHECK (fit.originalBands == 9);
    REQUIRE_FALSE (fit.bands.empty());
    CHECK (fit.bands.size() <= 6);
    checkOnSteps (fit.bands);
    for (std::size_t i = 1; i < fit.bands.size(); ++i)
        CHECK (fit.bands[i - 1].freq <= fit.bands[i].freq);

    // Close to the six-bell refit before rounding, and never boosting beyond the correction.
    const auto refit = refitBands (nine, fs, 6, false, bandwidthForQ (0.3));
    CHECK (fit.maxErrorDb < refit.maxErrorDb + 0.5);
    CHECK (fit.rmsErrorDb < refit.rmsErrorDb + 0.2);
    CHECK (fit.maxErrorDb < 2.5);
    const auto grid = refitGrid();
    const auto curve = responseDb (nine, grid, fs);
    const auto desk = responseDb (fit.bands, grid, fs);
    const auto [maxErr, rms] = fitError (curve, desk);
    CHECK (maxErr == doctest::Approx (fit.maxErrorDb));
    CHECK (rms == doctest::Approx (fit.rmsErrorDb));
    for (std::size_t n = 0; n < grid.size(); ++n)
        CHECK (desk[n] - std::max (curve[n], 0.0) <= refitBoostSlackDb + 0.1);
}

TEST_CASE ("x32: a correction that already fits is only rounded")
{
    const std::vector<Band> three { band (BandKind::bell, 97, -5.3, 4.1), band (BandKind::bell, 410, 1.7, 1.3),
                                    band (BandKind::bell, 3150, -2.2, 2.2) };
    const auto fit = x32::fitForDesk (three, fs);
    CHECK_FALSE (fit.refitted);
    CHECK (fit.bands.size() == 3);
    checkOnSteps (fit.bands);
    CHECK (fit.maxErrorDb < 0.4);   // a narrow cut between frequency steps 3.5% apart
    CHECK (x32::fitForDesk ({}, fs).bands.empty());
}

TEST_CASE ("x32: the snippet a desk loads")
{
    x32::Fit fit;
    fit.bands = { band (BandKind::bell, x32::snapFrequency (95.0), -5.5, x32::snapQ (4.3)),
                  band (BandKind::bell, x32::snapFrequency (4500.0), 1.75, x32::snapQ (1.0)) };
    const auto text = x32::snippet (fit, { x32::Strip::bus, 1 }, "Mains EQ");
    const auto l = lines (text);
    REQUIRE (l.size() == 8);
    CHECK (l[0].size() == 127);
    CHECK (l[0].rfind ("#4.0# \"Mains EQ\" 4 0 65536 0 1 ", 0) == 0);
    CHECK (l[1] == "/bus/01/eq ON");
    CHECK (l[2] == "/bus/01/eq/1 PEQ " + x32::frequencyToken (x32::snapFrequency (95.0)) + " -5.50 " + x32::qToken (x32::snapQ (4.3)));
    CHECK (l[3] == "/bus/01/eq/2 PEQ 4k53 +1.75 1.0");
    for (int k = 4; k < 8; ++k)
        CHECK (l[static_cast<std::size_t> (k)] == "/bus/01/eq/" + std::to_string (k - 1) + " PEQ 990.9 +0.00 2.0");
    CHECK (text.back() == '\n');
    CHECK (text.find ('\r') == std::string::npos);

    // The masks: bus n is bit 15 + n of the aux/bus mask (signed 32-bit), matrices and mains the main mask.
    const auto header = [&] (x32::Destination d) { return lines (x32::snippet (fit, d, "EQ"))[0].substr (0, 40); };
    CHECK (header ({ x32::Strip::bus, 16 }).rfind ("#4.0# \"EQ\" 4 0 -2147483648 0 1", 0) == 0);
    CHECK (header ({ x32::Strip::matrix, 3 }).rfind ("#4.0# \"EQ\" 4 0 0 4 1", 0) == 0);
    CHECK (header ({ x32::Strip::mainStereo, 1 }).rfind ("#4.0# \"EQ\" 4 0 0 64 1", 0) == 0);
    CHECK (header ({ x32::Strip::mainMono, 1 }).rfind ("#4.0# \"EQ\" 4 0 0 128 1", 0) == 0);
    CHECK (lines (x32::snippet (fit, { x32::Strip::matrix, 3 }, "EQ"))[1] == "/mtx/03/eq ON");
    CHECK (lines (x32::snippet (fit, { x32::Strip::mainStereo, 1 }, "EQ"))[2].rfind ("/main/st/eq/1 PEQ", 0) == 0);
    CHECK (lines (x32::snippet (fit, { x32::Strip::mainMono, 1 }, "EQ"))[7].rfind ("/main/m/eq/6 PEQ", 0) == 0);

    // The name: printable ASCII without quotes, at most 16 characters.
    CHECK (lines (x32::snippet (fit, {}, "A \"quoted\" name that is long"))[0].rfind ("#4.0# \"A quoted name th\" ", 0) == 0);
    CHECK (lines (x32::snippet (fit, {}, "\xc3\xa9"))[0].rfind ("#4.0# \"Room EQ\" ", 0) == 0);

    CHECK (x32::destinationName ({ x32::Strip::bus, 7 }) == "Bus 7");
    CHECK (x32::destinationName ({ x32::Strip::matrix, 2 }) == "Matrix 2");
    CHECK (x32::destinationName ({ x32::Strip::mainStereo, 1 }) == "Main LR");
    CHECK (x32::destinationName ({ x32::Strip::mainMono, 1 }) == "Main M/C");
}
