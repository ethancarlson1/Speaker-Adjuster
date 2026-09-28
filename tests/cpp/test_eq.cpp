#include "helpers.h"
#include "plugin/EqStages.h"
#include "plugin/LatestValue.h"

#include <doctest/doctest.h>

#include <algorithm>

using namespace roomeq;

namespace
{
// Runs x (mono, copied to both channels) through the stages; returns the left channel.
std::vector<double> run (EqStages& eq, const std::vector<double>& x, const EqSettings& s, int block = 256)
{
    std::vector<float> l (x.begin(), x.end()), r = l;
    for (std::size_t pos = 0; pos < l.size(); pos += static_cast<std::size_t> (block))
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (block), l.size() - pos));
        float* ch[] = { l.data() + pos, r.data() + pos };
        eq.process (ch, 2, n, s);
    }
    for (std::size_t i = 0; i < l.size(); ++i)
        REQUIRE (l[i] == r[i]);          // both sides get the same EQ
    return { l.begin(), l.end() };
}

std::vector<double> impulse (std::size_t n)
{
    std::vector<double> x (n, 0.0);
    x[0] = 1.0;
    return x;
}

CorrectionSet setOf (const std::vector<Band>& bands)
{
    CorrectionSet s;
    for (const auto& b : bands)
        s.bands[static_cast<std::size_t> (s.count++)] = b;
    return s;
}

// Energy above 8 kHz: a click (a jump in the waveform) shows up here; a smooth
// change to a 200 Hz sine doesn't.
double hfPeak (const std::vector<double>& y, double fs)
{
    auto hp = testing::highpass (8000.0, 0.7071, fs);
    const auto z = hp.process (hp.process (y));
    double peak = 0.0;
    for (std::size_t i = 1000; i < z.size(); ++i)   // skip the filter's own start-up
        peak = std::max (peak, std::abs (z[i]));
    return peak;
}

std::vector<double> sine (double f, double fs, std::size_t n, double amp = 0.5)
{
    std::vector<double> x (n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = amp * std::sin (2.0 * testing::pi * f * static_cast<double> (i) / fs);
    return x;
}
} // namespace

TEST_CASE ("state-variable sections match the RBJ biquads the graph draws")
{
    const std::vector<Band> bands { { BandKind::bell, 1000.0, -6.0, 2.0 }, { BandKind::bell, 60.0, 3.0, 0.7 },
                                    { BandKind::lowShelf, 100.0, 4.0 }, { BandKind::lowShelf, 300.0, -8.0, 1.2 },
                                    { BandKind::highShelf, 8000.0, -3.0 }, { BandKind::highPass, 80.0, 0.0, 0.7071 },
                                    { BandKind::lowPass, 12000.0, 0.0, 0.54 }, { BandKind::bell, 15000.0, -12.0, 4.0 } };
    for (double fs : { 44100.0, 48000.0, 96000.0 })
        for (const auto& b : bands)
        {
            EqStages eq;
            EqSettings s;
            eq.setCorrection (setOf ({ b }));
            eq.prepare (fs, s);
            const auto y = run (eq, impulse (1 << 16), s);
            for (double f : { 30.0, 90.0, 250.0, 1000.0, 3000.0, 9000.0, 16000.0 })
                CHECK (testing::dtftDb (y, f, fs) == doctest::Approx (bandDb (b, { f }, fs).front()).epsilon (1e-3));
        }
}

TEST_CASE ("correction amount, both stages and bypass")
{
    const double fs = 48000.0;
    const std::vector<Band> bands { { BandKind::bell, 150.0, -8.0, 1.0 }, { BandKind::highShelf, 6000.0, 3.0 } };
    EqSettings s;
    s.amount = 0.5;
    s.voicing[0] = { true, VoicingType::highPass24, 40.0, 0.0, 0.71 };
    s.voicing[3] = { true, VoicingType::bell, 400.0, -2.0, 1.4 };
    EqStages eq;
    eq.setCorrection (setOf (bands));
    eq.prepare (fs, s);
    const auto y = run (eq, impulse (1 << 16), s);

    std::vector<Band> expected;
    for (const auto& b : bands)
        expected.push_back (b.scaled (0.5));
    for (const auto& v : s.voicing)
    {
        const auto sec = voicingSections (v);
        for (int i = 0; i < sec.count; ++i)
            expected.push_back (sec.bands[static_cast<std::size_t> (i)]);
    }
    CHECK (expected.size() == 5);   // 2 correction + HP24 (two sections) + bell
    for (double f : { 25.0, 40.0, 150.0, 400.0, 2000.0, 12000.0 })
        CHECK (testing::dtftDb (y, f, fs) == doctest::Approx (responseDb (expected, { f }, fs).front()).epsilon (1e-3));

    // Both stages bypassed: after the glide the output is the input, bit for bit.
    s.correctionOn = false;
    s.voicingOn = false;
    const auto x = testing::whiteNoise (48000, 0.1, 3);
    const auto z = run (eq, x, s);
    for (std::size_t i = 24000; i < x.size(); ++i)
        REQUIRE (z[i] == static_cast<double> (static_cast<float> (x[i])));
}

TEST_CASE ("nothing to do passes audio through untouched")
{
    EqStages eq;
    EqSettings s;
    eq.prepare (48000.0, s);
    const auto x = testing::whiteNoise (4096, 0.3, 4);
    const auto y = run (eq, x, s);
    for (std::size_t i = 0; i < x.size(); ++i)
        REQUIRE (y[i] == static_cast<double> (static_cast<float> (x[i])));
}

TEST_CASE ("applying a correction glides instead of clicking")
{
    const double fs = 48000.0;
    const auto x = sine (200.0, fs, 48000);
    const CorrectionSet deep = setOf ({ { BandKind::bell, 200.0, -12.0, 2.0 }, { BandKind::lowShelf, 120.0, -6.0 } });

    // Smooth: the new correction arrives mid-stream and glides.
    EqStages eq;
    EqSettings s;
    eq.prepare (fs, s);
    std::vector<float> l (x.begin(), x.end()), r = l;
    for (std::size_t pos = 0; pos < l.size(); pos += 256)
    {
        if (pos == 12032)
            eq.setCorrection (deep);
        float* ch[] = { l.data() + pos, r.data() + pos };
        eq.process (ch, 2, static_cast<int> (std::min<std::size_t> (256, l.size() - pos)), s);
    }
    const std::vector<double> smooth (l.begin(), l.end());

    // Hard switch at the same point, for comparison.
    EqStages hard;
    hard.prepare (fs, s);
    std::vector<double> y (x.begin(), x.begin() + 12032);
    EqStages after;
    after.setCorrection (deep);
    after.prepare (fs, s);
    std::vector<double> rest (x.begin() + 12032, x.end());
    const auto tail = run (after, rest, s);
    y.insert (y.end(), tail.begin(), tail.end());

    const auto clickHf = hfPeak (y, fs);
    const auto glideHf = hfPeak (smooth, fs);
    CHECK (clickHf > 1e-3);
    CHECK (glideHf < 1e-4);
    CHECK (glideHf < clickHf / 30.0);

    // And it gets there: the 200 Hz level settles at the correction's response.
    const std::vector<double> settled (smooth.end() - 4800, smooth.end());
    const auto level = *std::max_element (settled.begin(), settled.end());
    const auto expectedDb = responseDb ({ deep.bands[0], deep.bands[1] }, { 200.0 }, fs).front();
    CHECK (20.0 * std::log10 (level / 0.5) == doctest::Approx (expectedDb).epsilon (0.01));
}

TEST_CASE ("latest-value hand-off keeps only the newest value")
{
    LatestValue<int> v;
    int out = -1;
    CHECK_FALSE (v.read (out));
    v.write (1);
    v.write (2);
    v.write (3);
    CHECK (v.read (out));
    CHECK (out == 3);
    CHECK_FALSE (v.read (out));
    v.write (4);
    CHECK (v.read (out));
    CHECK (out == 4);
}
