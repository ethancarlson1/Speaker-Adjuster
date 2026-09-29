#pragma once

// Capture grading: per-octave-band SNR and repeat-sweep consistency.
// Port of prototype/roomeq/grading.py.

#include <optional>
#include <string>
#include <vector>

namespace roomeq
{
enum class Grade
{
    pass = 0,
    marginal = 1,
    redo = 2
};

const char* gradeLabel (Grade g);   // "pass", "marginal", "redo"

struct GradingConfig
{
    double snrPassDb = 20.0;
    double snrMarginalDb = 10.0;
    double consistencyPassDb = 1.0;
    double consistencyMarginalDb = 3.0;
    double outOfRangeDb = 10.0;     // a band this far below the passband is outside the PA's range
    double passbandLo = 250.0;
    double passbandHi = 4000.0;
};

struct BandResult
{
    std::string name;
    double center = 0.0, lo = 0.0, hi = 0.0;
    double levelDb = 0.0;                    // band level relative to the passband level
    double snrDb = 0.0;
    std::optional<double> spreadDb;          // repeat-to-repeat spread in the worst 1/3 octave
    std::optional<double> excessSpreadDb;    // ... minus what the measured noise can explain (graded)
    bool outOfRange = false;
    Grade snrGrade = Grade::pass;
    Grade consistencyGrade = Grade::pass;

    std::optional<Grade> grade() const;      // empty when out of range
};

struct CaptureGrade
{
    std::vector<BandResult> bands;
    Grade overall = Grade::redo;
    std::vector<std::string> reasons;
    std::vector<std::string> notes;
};

// signalPower / noisePower are per-bin powers in the same (|H|^2) units.
// repeatPowers holds |H_i|^2 for each repeat when there are two or more.
// fMax clips the top band (sweep f2 / Nyquist).
CaptureGrade gradeCapture (const std::vector<double>& freqs, const std::vector<double>& signalPower,
                           const std::vector<double>& noisePower,
                           const std::vector<std::vector<double>>* repeatPowers,
                           const GradingConfig& cfg, double fMax);

// The same capture graded against other settings (a sub's reference band):
// exactly what gradeCapture would give with them. SNR and repeat spread don't
// depend on the band, so they're kept; the levels, the out-of-range ends and
// the verdict are redone. Notes that didn't come from grading (clock drift) are kept.
CaptureGrade regrade (const CaptureGrade& grade, const std::vector<double>& freqs, const std::vector<double>& signalPower,
                      const GradingConfig& cfg);
} // namespace roomeq
