#pragma once

// The loudness compensation stage, after correction and voicing. JUCE-free, so
// it's unit tested.
//
// Per block (audio thread): track the C-weighted level of what arrives (before
// this stage's own EQ, so the boost never feeds back into the reading), map it
// to SPL with the calibration, hold it through the deadband, and set the low /
// high shelves (and the optional tracking high-pass) for that level. Changes
// glide. While a measurement runs, the stage is flat and doesn't track, so test
// signals are neither compensated nor counted.
//
// A tap records the final output (mono) and the mic for calibration, the
// re-check and the mic calibrator; it's handed over like CaptureRecorder's
// requests (the audio thread never allocates, locks or waits).

#include "plugin/EqStages.h"
#include "plugin/LatestValue.h"
#include "roomeq/loudness.h"

#include <atomic>
#include <memory>
#include <vector>

// Everything the audio thread reads from the parameters, once per block.
struct LoudnessSettings
{
    bool on = true;
    bool useMic = false;
    double hpBaseHz = 40.0;          // the PA's measured low-end roll-off
    roomeq::LoudnessConfig config;
};

// The stage's shelf plan and calibration as the audio thread receives them.
struct LoudnessModel
{
    roomeq::ShelfPlan plan;
    bool hasPlan = false;
    roomeq::Calibration calibration;
    bool calibrated = false;
};

struct TapRequest
{
    std::vector<float> output, mic;   // sized by the message thread
    std::size_t skip = 0;             // samples to let pass before recording
    std::size_t position = 0;         // audio-thread cursor (skip counted first)
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> finished { false };
    bool cancelled = false;           // valid once finished
};

class LoudnessStage
{
public:
    // Audio stopped.
    void prepare (double sampleRate, int maxBlockSize, const LoudnessSettings& settings);

    // Message thread (lock-free).
    void setModel (const LoudnessModel& newModel) noexcept { pendingModel.write (newModel); }
    bool startTap (std::unique_ptr<TapRequest> request);
    void cancelTap();
    bool isTapBusy() const { return tap != nullptr; }
    std::unique_ptr<TapRequest> collectTap();
    void abortTapWhileStopped();      // prepareToPlay / releaseResources only
    float getTapProgress() const;

    // What the stage is doing, for the UI (any thread).
    struct Status
    {
        std::atomic<bool> hasLevel { false };
        std::atomic<float> splNow { 0.0f };       // tracked SPL (dB C), from the chosen source
        std::atomic<float> splUsed { 0.0f };      // the deadband level the shelves follow
        std::atomic<float> lowGainDb { 0.0f }, highGainDb { 0.0f };
        std::atomic<float> hpFreq { 0.0f };       // 0 when the high-pass is off
        std::atomic<float> lowFreq { 0.0f }, lowQ { 0.0f }, highFreq { 0.0f }, highQ { 0.0f };
    };
    const Status& getStatus() const { return status; }

    // Audio thread. `mic` may be null. `suspend` while a measurement runs.
    void process (float* const* channels, int numChannels, const float* mic, int numSamples,
                  const LoudnessSettings& settings, bool suspend) noexcept;

private:
    void updateTargets (const LoudnessSettings& settings, bool suspend) noexcept;
    void recordTap (float* const* channels, int numChannels, const float* mic, int numSamples) noexcept;

    double fs = 48000.0;
    LatestValue<LoudnessModel> pendingModel;
    LoudnessModel model;
    roomeq::LevelTracker outputTracker, micTracker;
    roomeq::Deadband deadband;
    EqChain chain;
    std::vector<float> mono;
    LoudnessSettings last;
    bool lastSuspend = true;
    bool haveTargets = false;
    double lowGain = 0.0, highGain = 0.0;

    std::unique_ptr<TapRequest> tap;                // owned by the message thread
    std::atomic<TapRequest*> activeTap { nullptr }; // written by the audio thread while set

    Status status;
};

// The stage's current EQ as bands (for the graph and tests), from its status.
std::vector<roomeq::Band> loudnessBands (const LoudnessStage::Status& status);
