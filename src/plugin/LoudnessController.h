#pragma once

#include "plugin/LoudnessStage.h"
#include "plugin/MeasurementEngine.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Message-thread side of loudness compensation:
//
// - Plans the shelves whenever the reference level or sample rate changes.
// - Level calibration: pink noise on both speakers through the EQ (the stage is
//   flat meanwhile); the stage's tap records the output level and, with a mic,
//   the mic level and the output-to-mic transfer per third octave. The user then
//   types in the SPL their meter read (dB C, slow), or accepts the one a
//   calibrated mic suggests.
// - Mic calibrator: the mic's level with a 94 / 114 dB calibrator on it.
// - Re-check: ~12 s of the show through the mic; if the system got louder or
//   quieter after the plugin (amp gain, a fader), the calibration moves by that much.
// - Publishes the plan and calibration to the stage, and saves them with the session.
//
// Analysis runs on a background thread; nothing here blocks the audio thread.
class LoudnessController : public juce::ChangeBroadcaster,
                           private juce::Timer
{
public:
    // What the controller polls from the processor.
    struct Environment
    {
        double fs = 0.0;                   // 0 while audio isn't running
        double referenceSpl = 95.0;
        bool micConnected = false;
        bool micSignal = false;            // and it carried signal within the last second
        double signalLevelDbfs = -12.0;    // calibration noise peak level
        bool available = true;             // false in the standalone app
    };

    enum class Step
    {
        idle,
        calibrating,       // noise playing, tap recording
        awaitingSpl,       // noise done: waiting for the meter reading
        micCalibrating,
        rechecking
    };

    struct Info
    {
        bool calibrated = false;
        roomeq::Calibration calibration;
        juce::int64 calibratedAt = 0;      // ms since the epoch
        bool canRecheck = false;           // the mic heard the calibration noise
        bool hasMicOffset = false;
        double micOffsetDb = 0.0;          // SPL = mic level (C-weighted dBFS) + offset
        double calibratorSpl = 94.0;
        juce::int64 micCalibratedAt = 0;
        // While awaiting the SPL.
        double measuredOutputDbfs = 0.0;
        std::optional<double> suggestedSpl;
        // The last re-check that moved the calibration.
        juce::int64 recheckedAt = 0;
        double recheckChangeDb = 0.0;
    };

    static constexpr double calibrationNoiseSeconds = 12.0;
    static constexpr double calibrationSkipSeconds = 1.5;     // the EQ and room settle
    static constexpr double calibrationRecordSeconds = 10.0;
    static constexpr double micCalibrationSeconds = 3.0;
    static constexpr double recheckSeconds = 12.0;
    static constexpr double minRecheckChangeDb = 0.5;         // smaller changes leave the calibration alone

    LoudnessController (LoudnessStage& stageToUse, MeasurementEngine& engineToUse);
    ~LoudnessController() override;

    void setEnvironmentSource (std::function<Environment()> source) { environmentSource = std::move (source); }

    juce::Result startCalibration();
    juce::Result setMeasuredSpl (double spl);          // finishes a calibration
    juce::Result startMicCalibration (double calibratorSpl);
    juce::Result startRecheck();
    void cancel();
    // Forgets the level calibration (and its re-checks): the shelves and
    // high-pass go flat until it's calibrated again. The mic calibration is the
    // mic's, not the room's, so it stays. Only while idle or awaiting the SPL.
    void clearCalibration();

    Step getStep() const { return step; }
    bool isAnalysing() const { return analysing; }
    float getProgress() const;
    juce::String getStatus() const { return status; }
    Info getInfo() const;
    bool hasPlan() const { return planned; }

    // The PA's low-end roll-off (the correction's fit range), for the tracking high-pass. Any thread.
    double getHighpassBase() const noexcept { return hpBase.load (std::memory_order_relaxed); }

    // Persistence (safe from any thread).
    juce::ValueTree toValueTree() const;
    void fromValueTree (const juce::ValueTree& tree);

    // Driven by a timer; public so tests can pump it.
    void update();

    static inline const juce::Identifier treeType { "Loudness" };

private:
    struct Analysis
    {
        Step step = Step::idle;
        double outputDbfs = 0.0;
        double micDbfs = 0.0;
        bool micHeard = false;
        std::vector<double> bands;
    };

    // Shared with background jobs so they can finish safely after we're gone.
    struct Mailbox
    {
        std::mutex lock;
        std::vector<Analysis> results;
    };

    void timerCallback() override { update(); }
    juce::Result startTap (Step newStep, double skipSeconds, double recordSeconds, bool withMic);
    void tapFinished (std::unique_ptr<TapRequest> tap);
    void handle (const Analysis& a);
    void publishModel();
    Environment environment() const;

    LoudnessStage& stage;
    MeasurementEngine& engine;
    std::function<Environment()> environmentSource;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    juce::ThreadPool pool { juce::ThreadPoolOptions {}.withThreadName ("Loudness analysis").withNumberOfThreads (1) };

    Step step = Step::idle;
    bool analysing = false;
    bool resumeAwaiting = false;        // mic calibration started while a calibration awaited its SPL
    double tapFs = 48000.0;
    double requestedCalibratorSpl = 94.0;
    juce::String status, stopReason;

    roomeq::ShelfPlan plan;
    bool planned = false;
    double planFs = 0.0, planRef = 0.0;
    std::atomic<double> hpBase { 40.0 };
    std::atomic<bool> modelDirty { true };

    // Persisted; guarded for readers on other threads.
    mutable std::mutex stateLock;
    bool calibrated = false;
    roomeq::Calibration calibration;
    std::vector<double> calibrationBands;   // output-to-mic per re-check band (NaN where unknown)
    juce::int64 calibratedAt = 0;
    bool hasMicOffset = false;
    double micOffsetDb = 0.0, calibratorSpl = 94.0;
    juce::int64 micCalibratedAt = 0;
    juce::int64 recheckedAt = 0;
    double recheckChangeDb = 0.0;

    // A calibration waiting for its SPL.
    Analysis pending;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessController)
};
