#pragma once

// Level calibration, level tracking and ISO 226 loudness compensation.
// Port of prototype/roomeq/loudness.py; see there for the reasoning.
//
// The tracker and deadband allocate nothing after prepare(), so the audio
// thread can run them. The shelf plan and the re-check are message-thread work.

#include "roomeq/filters.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace roomeq
{
struct LoudnessConfig
{
    double referenceSpl = 95.0;     // dB(C) at the mix position where the system sounds right
    double amount = 1.0;
    double maxLowDb = 8.0;
    double maxHighDb = 4.0;
    double lowCeilingDb = 12.0;     // fixed: never more low boost than this
    double speedS = 30.0;           // level averaging time constant, both ways
    double attackS = 1.0;           // a big jump up is followed this fast (energy average)...
    double jumpDb = 6.0;            // ...once the recent level is this far above the average...
    double jumpRecentS = 3.0;       // ...("recent": an energy average over this long)...
    double jumpFollowS = 4.0;       // ...and for this long after
    double windowS = 0.4;           // momentary level window
    double absGateDbfs = -70.0;     // below this (C-weighted) it's silence
    double relGateDb = 20.0;        // this far below the estimate it's a pause...
    double pauseHoldS = 8.0;        // ...unless it lasts longer than this
    bool hpTrack = false;
    double hpRiseOctaves = 0.5;
    double deadbandDb = 2.0;        // level changes smaller than this don't move the EQ...
    double driftS = 30.0;           // ...except by a slow drift that lands it on the level
};

// IEC 61672 C-weighting as two RBJ sections (Q 0.5) plus a gain for 0 dB at 1 kHz.
std::array<Band, 2> cWeightingBands();
double cWeightingGain (double fs);
double cWeightedLevelDbfs (const std::vector<double>& x, double fs);

// ISO 226 contour difference (dB) keeping the balance heard at the reference; zero at or above it.
std::vector<double> compensationTarget (const std::vector<double>& freqs, double currentSpl, double referenceSpl);

// Shelf frequency/Q fitted once per reference level, gains tabulated per dB below it (0..60).
struct ShelfPlan
{
    static constexpr int numDeltas = 61;
    double referenceSpl = 95.0;
    double lowFreq = 134.0, lowQ = 0.4, highFreq = 11300.0, highQ = shelfQ;
    std::array<double, numDeltas> lowGain {}, highGain {};   // amount 100%, no limits
};

ShelfPlan planShelves (double referenceSpl, double fs);

// Low and high shelf gains (dB) at the current level, with amount and limits applied.
std::pair<double, double> shelfGains (const ShelfPlan& plan, double currentSpl, const LoudnessConfig& cfg);

// 24 dB/octave Butterworth at the PA's roll-off, up to hpRiseOctaves higher at the max low boost.
std::array<Band, 2> trackingHighpass (double baseHz, double lowGainDb, const LoudnessConfig& cfg);

// Output level (C-weighted dBFS at the loudness stage's input) that gave `spl` dB(C) at the mix position.
struct Calibration
{
    double outputDbfs = 0.0;
    double spl = 0.0;
    bool hasMic = false;
    double micDbfs = 0.0;           // mic level (C-weighted) during calibration, for mic tracking

    double splFromOutput (double levelDbfs) const { return spl + levelDbfs - outputDbfs; }
    double splFromMic (double levelDbfs) const { return spl + levelDbfs - micDbfs; }
};

// Block-by-block level estimate (C-weighted dBFS), as the audio thread runs it.
class LevelTracker
{
public:
    void prepare (double fs, const LoudnessConfig& cfg);
    void setConfig (const LoudnessConfig& cfg) noexcept { config = cfg; }
    void reset() noexcept;

    // Feeds a block; returns how many windows completed. With `follow`, a window
    // only counts when the followed tracker's matching window did (mic tracking:
    // only while program plays). Feed `follow` the same block first.
    int process (const float* x, int numSamples, const LevelTracker* follow = nullptr) noexcept;

    bool hasEstimate() const noexcept { return has; }
    double estimate() const noexcept { return value; }
    bool lastActive() const noexcept { return active; }
    bool isJumping() const noexcept { return jumpLeft > 0.0; }   // following a big jump up

private:
    bool update (double momentary, bool gates) noexcept;

    LoudnessConfig config;
    Biquad hp, lp;
    double gain = 1.0;
    double hp1 = 0.0, hp2 = 0.0, lp1 = 0.0, lp2 = 0.0;   // TDF-II state
    long window = 19200;
    long count = 0;
    double acc = 0.0;
    bool has = false, active = false;
    double value = 0.0;
    double recent = 0.0;            // the short average the jump guard watches
    double jumpLeft = 0.0;          // seconds of following a big jump up still to go
    double gatedFor = 0.0;
};

// The level the compensation uses: moves only when the estimate pushes more than
// deadbandDb past it, otherwise drifts towards it (driftS). Once per window.
class Deadband
{
public:
    void reset() noexcept { has = false; }
    double update (double estimate, const LoudnessConfig& cfg) noexcept;
    bool hasLevel() const noexcept { return has; }
    double level() const noexcept { return held; }

private:
    bool has = false;
    double held = 0.0;
};

// Re-check: output-to-mic gain per third octave (63 Hz-8 kHz), NaN where the
// program didn't excite the band or the mic didn't hear it coherently.
const std::vector<double>& recheckBands();
std::vector<double> transferBandsDb (const std::vector<double>& output, const std::vector<double>& mic, double fs,
                                     double minCoherence = 0.3);
std::optional<double> recheckGainChange (const std::vector<double>& calibrationBands, const std::vector<double>& nowBands,
                                         int minBands = 6);
Calibration recalibrated (const Calibration& cal, double gainChangeDb);
} // namespace roomeq
