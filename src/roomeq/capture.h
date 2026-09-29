#pragma once

// One capture = one mic position: 1-3 sweeps (or a program excerpt) -> graded response.
// Port of prototype/roomeq/capture.py.

#include "roomeq/dualfft.h"
#include "roomeq/grading.h"
#include "roomeq/spectrum.h"
#include "roomeq/sweep.h"

#include <limits>
#include <string>
#include <vector>

namespace roomeq
{
struct AnalysisConfig
{
    WindowConfig window;
    GradingConfig grading;
    double maxDelaySeconds = 1.0;   // longest loop delay searched (latency + flight time)
    double noiseMarginSeconds = 0.05;
};

struct Capture
{
    std::string name;
    std::string kind;                         // "sweep" or "program"
    double fs = 0.0;
    std::vector<double> freqs;                // linear FFT grid
    std::vector<double> power;                // |H|^2 (repeats coherently averaged)
    std::vector<double> noisePower;           // noise estimate in |H|^2 units
    std::vector<double> weight;               // 0/1: 0 outside the band / where program had no energy
    CaptureGrade grade;
    std::vector<std::vector<double>> repeatPowers;
    std::vector<double> delaysMs;
    std::vector<double> ir;                   // windowed, aligned, averaged IR (sweeps only)
    bool excluded = false;
    double driftPpm = std::numeric_limits<double>::quiet_NaN();   // program: output/mic clock difference corrected (0 if none)
};

std::vector<double> inBand (const std::vector<double>& freqs, double f1, double f2);

struct WindowedResponse
{
    std::vector<double> freqs, power;
    std::size_t peak = 0;
};

// |H|^2 of a known IR through exactly the window a capture uses, around its own peak.
WindowedResponse windowedResponse (const std::vector<double>& ir, double fs, const AnalysisConfig& cfg = {});

// Deconvolve each repeat, line them up, average coherently, grade. Each recording
// is [preroll | sweep | tail]. Throws std::runtime_error if the tail is too short
// for the detected loop delay.
Capture analyzeSweepCapture (const std::string& name, const std::vector<std::vector<double>>& recordings,
                             const SweepConfig& sweepCfg, const AnalysisConfig& cfg = {});

// Dual-FFT estimate against program material, graded with the same rules.
Capture analyzeProgramCapture (const std::string& name, const std::vector<double>& reference,
                               const std::vector<double>& mic, double fs, const AnalysisConfig& cfg = {},
                               const DualFFTConfig& dualCfg = {});
} // namespace roomeq
