#include "roomeq/quality.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace roomeq;

namespace
{
BandResult band (const char* name, double centre, double snrDb, std::optional<double> excess = {}, std::optional<double> coherence = {})
{
    BandResult b;
    b.name = name;
    b.center = centre;
    b.lo = centre / std::sqrt (2.0);
    b.hi = centre * std::sqrt (2.0);
    b.snrDb = snrDb;
    b.excessSpreadDb = excess;
    b.spreadDb = excess;
    b.coherence = coherence;
    return b;
}

// A flat response from 20 Hz to 20 kHz with the given grading.
Capture flat (std::vector<BandResult> bands, Grade overall = Grade::pass)
{
    Capture c;
    c.fs = 48000.0;
    c.freqs = rfftFreqs (32768, c.fs);
    c.power.assign (c.freqs.size(), 1.0);
    c.noisePower.assign (c.freqs.size(), 1e-4);
    c.weight = inBand (c.freqs, 20.0, 20000.0);
    c.grade.bands = std::move (bands);
    c.grade.overall = overall;
    return c;
}
} // namespace

TEST_CASE ("ratings follow the grading's limits")
{
    CHECK (rate (35.0, snrLimitsDb) == Rating::excellent);
    CHECK (rate (30.0, snrLimitsDb) == Rating::excellent);
    CHECK (rate (20.0, snrLimitsDb) == Rating::good);
    CHECK (rate (10.0, snrLimitsDb) == Rating::fair);
    CHECK (rate (9.9, snrLimitsDb) == Rating::poor);
    CHECK (rate (0.5, repeatLimitsDb, false) == Rating::excellent);
    CHECK (rate (1.0, repeatLimitsDb, false) == Rating::good);
    CHECK (rate (3.0, repeatLimitsDb, false) == Rating::fair);
    CHECK (rate (3.1, repeatLimitsDb, false) == Rating::poor);
    const GradingConfig g;
    CHECK (snrLimitsDb.good == g.snrPassDb);
    CHECK (snrLimitsDb.fair == g.snrMarginalDb);
    CHECK (std::string (ratingLabel (Rating::fair)) == "Fair");
}

TEST_CASE ("the worst graded band sets each rating, and the confidence follows")
{
    auto bands = std::vector<BandResult> { band ("63", 63.0, 34.0, 0.2), band ("1k", 1000.0, 26.0, 0.8), band ("8k", 8000.0, 31.0, 0.1) };
    bands.push_back (band ("16k", 16000.0, 2.0));
    bands.back().outOfRange = true;   // out of the PA's range: not graded
    const auto q = measurementQuality (flat (bands));
    CHECK (q.snrDb == 26.0);
    CHECK (q.snrBand == "1k");
    CHECK (q.snr == Rating::good);
    CHECK (q.repeatability == Rating::good);
    CHECK (q.repeatBand == "1k");
    CHECK_FALSE (q.coherence.has_value());
    CHECK (q.confidence == Confidence::high);
    CHECK (q.reasons.empty());
    CHECK (q.usable.first == doctest::Approx (20.0));

    auto noisy = bands;
    noisy[0].snrDb = 14.0;
    const auto fair = measurementQuality (flat (noisy, Grade::marginal));
    CHECK (fair.confidence == Confidence::medium);
    REQUIRE (fair.reasons.size() == 1);
    CHECK (fair.reasons[0] == "signal-to-noise fair: 14 dB at 63 Hz");

    auto moved = bands;
    moved[2].excessSpreadDb = 4.2;
    const auto poor = measurementQuality (flat (moved, Grade::redo));
    CHECK (poor.confidence == Confidence::low);
    REQUIRE (poor.reasons.size() == 2);
    CHECK (poor.reasons[0] == "repeatability poor: repeats 4.2 dB apart at 8k Hz");
    CHECK (poor.reasons[1] == "graded redo");
}

TEST_CASE ("music has coherence and no repeatability; nothing heard is low")
{
    const auto q = measurementQuality (flat ({ band ("125", 125.0, 24.0, {}, 0.97), band ("2k", 2000.0, 22.0, {}, 0.8) }));
    REQUIRE (q.coherence.has_value());
    CHECK (*q.coherence == 0.8);
    CHECK (q.coherenceRating == Rating::fair);
    CHECK_FALSE (q.repeatability.has_value());
    CHECK (q.confidence == Confidence::medium);

    auto silent = band ("1k", 1000.0, 0.0);
    silent.outOfRange = true;
    const auto none = measurementQuality (flat ({ silent }, Grade::redo));
    CHECK (none.confidence == Confidence::low);
    CHECK (none.reasons == std::vector<std::string> { "no band was heard clearly" });
}
