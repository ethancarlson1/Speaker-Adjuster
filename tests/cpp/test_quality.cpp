#include "roomeq/quality.h"

#include <doctest/doctest.h>

#include <algorithm>
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

TEST_CASE ("the correction explains where it held back")
{
    // A hand-made fit state on a 1/3-octave grid: the PA stops at 50 Hz; a dip
    // at 200 Hz; the positions disagree at 1 kHz; the target asks for +6 dB
    // near 4 kHz (Max boost 3) and -15 dB near 8 kHz (Max cut 12).
    CorrectionResult r;
    r.grid = logFreqGrid (20.0, 20000.0, 3);
    const auto n = r.grid.size();
    r.averageDb.assign (n, 0.0);
    r.targetDb.assign (n, 0.0);
    r.dipDb.assign (n, 0.0);
    r.spreadDb.assign (n, 2.0);
    r.nullMask.assign (n, false);
    r.correctionDb.assign (n, 0.0);
    r.paRange = { 50.0, r.grid.back() };
    r.fitRange = { 50.0, r.grid.back() };
    r.maxCutDb = 12.0;
    r.maxBoostDb = 3.0;
    const auto at = [&] (double f)
    {
        std::size_t best = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (std::abs (std::log (r.grid[i] / f)) < std::abs (std::log (r.grid[best] / f)))
                best = i;
        return best;
    };
    r.averageDb[at (200.0)] = -9.0;
    r.dipDb[at (200.0)] = -9.0;
    r.nullMask[at (200.0)] = true;
    r.averageDb[at (1000.0)] = -4.0;
    r.spreadDb[at (1000.0)] = 8.0;
    r.nullMask[at (1000.0)] = true;
    r.averageDb[at (4000.0)] = -6.0;
    r.averageDb[at (8000.0)] = 15.0;

    SessionSummary summary;
    summary.nGood = 3;
    CorrectionConfig cfg;
    const auto e = explainCorrection ({}, summary, r, cfg);
    REQUIRE (e.size() == 5);
    CHECK (e[0].kind == "bandwidth");
    CHECK (e[0].text.rfind ("Not correcting below 50 Hz: the system is more than 6 dB down there", 0) == 0);
    CHECK (e[1].kind == "null");
    CHECK (e[1].text.rfind ("Not boosting the dip at 200 Hz (9 dB below the trend)", 0) == 0);
    CHECK (e[2].kind == "disagreement");
    CHECK (e[2].text.rfind ("Not boosting around 1 kHz: the positions disagree by up to 8 dB", 0) == 0);
    CHECK (e[3].text == "Boost held to +3.0 dB at 4.1 kHz (Max boost): the response is 6.0 dB below the target there.");
    CHECK (e[4].text == "Cut held to -12.0 dB at 8.1 kHz (Max cut): the response is 15.0 dB above the target there.");

    // Few positions: quick mode says so first, and a capped boost names the cap.
    summary.nGood = 1;
    summary.policy = { 2, 0.5, 3.0 };
    r.maxBoostDb = 3.0;   // min (3, cap 3 / strength 0.5)
    r.strength = 0.5;
    const auto quick = explainCorrection ({}, summary, r, cfg);
    CHECK (quick[0].text == "Only 1 good position: the fit smooths to 1/2 octave and is held to 50% strength, at most 3 dB. "
                            "Measure 2 more to correct fully.");
    const auto boost = std::find_if (quick.begin(), quick.end(), [] (const auto& x) { return x.text.rfind ("Boost held", 0) == 0; });
    REQUIRE (boost != quick.end());
    CHECK (boost->text.find ("+1.5 dB at 4.1 kHz (the quick-mode cap)") != std::string::npos);
}
