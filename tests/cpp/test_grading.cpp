#include "roomeq/fft.h"
#include "roomeq/grading.h"

#include <doctest/doctest.h>

#include <functional>

using namespace roomeq;

namespace
{
const auto freqs = rfftFreqs (32768, 48000.0);
const std::vector<double> flat (freqs.size(), 1.0);

std::vector<double> noiseWith (double level, const std::function<bool (double)>& where)
{
    std::vector<double> n (freqs.size(), 1e-4);   // 40 dB SNR baseline
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (where (freqs[k]))
            n[k] = level;
    return n;
}

CaptureGrade grade (const std::vector<double>& signal, const std::vector<double>& noise,
                    const std::vector<std::vector<double>>* repeats = nullptr, const GradingConfig& cfg = {})
{
    return gradeCapture (freqs, signal, noise, repeats, cfg, 20000.0);
}
} // namespace

TEST_CASE ("clean capture passes")
{
    const auto g = grade (flat, noiseWith (1e-4, [] (double) { return false; }));
    CHECK (g.overall == Grade::pass);
    REQUIRE (g.reasons.size() == 1);
    CHECK (g.reasons[0] == "all bands \xE2\x89\xA5 20 dB SNR");
}

TEST_CASE ("low-end noise asks for a redo with the range")
{
    const auto g = grade (flat, noiseWith (0.3, [] (double f) { return f < 88.0; }));
    CHECK (g.overall == Grade::redo);
    REQUIRE (g.reasons.size() == 1);
    CHECK (g.reasons[0] == "redo: low-end noise too high below 88 Hz (SNR 5 dB)");
}

TEST_CASE ("high-frequency and mid-band wording")
{
    const auto hf = grade (flat, noiseWith (0.05, [] (double f) { return f > 5657.0; }));
    CHECK (hf.overall == Grade::marginal);
    CHECK (hf.reasons.at (0) == "marginal: high-frequency noise too high above 5.7 kHz (SNR 13 dB)");

    const auto mid = grade (flat, noiseWith (0.05, [] (double f) { return f > 354.0 && f < 707.0; }));
    CHECK (mid.reasons.at (0) == "marginal: noise too high 350 Hz\xE2\x80\x93" "710 Hz (SNR 13 dB)");
}

TEST_CASE ("bands below the PA's range are not graded")
{
    auto signal = flat;
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (freqs[k] < 44.0)
            signal[k] = 0.01;
    const auto g = grade (signal, noiseWith (0.01, [] (double f) { return f < 44.0; }));
    CHECK (g.bands[0].outOfRange);
    CHECK_FALSE (g.bands[0].grade().has_value());
    CHECK (g.overall == Grade::pass);
    REQUIRE (g.notes.size() == 1);
    CHECK (g.notes[0] == "below the PA's range under 44 Hz (not graded)");

    const auto noisy = grade (signal, noiseWith (0.3, [] (double f) { return f < 88.0; }));
    CHECK (noisy.reasons.at (0) == "redo: low-end noise too high below 88 Hz (SNR 5 dB)");
}

TEST_CASE ("a repeat mismatch in one third-octave is caught")
{
    auto bumped = flat;
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (freqs[k] >= 891.0 && freqs[k] < 1122.0)
            bumped[k] *= std::pow (10.0, 0.2);
    const std::vector<std::vector<double>> repeats { flat, bumped };
    const auto g = grade (flat, noiseWith (1e-4, [] (double) { return false; }), &repeats);
    REQUIRE (g.bands[5].spreadDb.has_value());
    CHECK (*g.bands[5].spreadDb == doctest::Approx (2.0).epsilon (0.02));
    CHECK (g.overall == Grade::marginal);
    REQUIRE (g.reasons.size() == 1);
    CHECK (g.reasons[0].rfind ("marginal: repeat sweeps differ by 2.0 dB 710 Hz\xE2\x80\x93" "1.4 kHz", 0) == 0);
}

TEST_CASE ("inconsistency explained by noise is not reported twice")
{
    auto wobble = flat;
    for (std::size_t k = 0; k < freqs.size(); ++k)
        if (freqs[k] < 88.0)
            wobble[k] *= std::pow (10.0, 0.4);
    const std::vector<std::vector<double>> repeats { flat, wobble };
    const auto g = grade (flat, noiseWith (0.3, [] (double f) { return f < 88.0; }), &repeats);
    CHECK (g.overall == Grade::redo);
    REQUIRE (g.reasons.size() == 1);
    CHECK (g.reasons[0] == "redo: low-end noise too high below 88 Hz (SNR 5 dB)");
}

TEST_CASE ("thresholds are configurable")
{
    GradingConfig strict;
    strict.snrPassDb = 45.0;
    strict.snrMarginalDb = 30.0;
    CHECK (grade (flat, noiseWith (1e-4, [] (double) { return false; }), nullptr, strict).overall == Grade::marginal);
}
