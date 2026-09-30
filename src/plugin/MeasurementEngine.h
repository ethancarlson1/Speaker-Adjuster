#pragma once

#include "plugin/CaptureRecorder.h"
#include "roomeq/alignment.h"
#include "roomeq/averaging.h"
#include "roomeq/capture.h"
#include "roomeq/correction.h"
#include "roomeq/targets.h"

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Message-thread side of measurement and correction: starts measurements,
// hands finished recordings to a background thread for analysis, keeps the
// capture list, recomputes the averaged response and the proposed correction
// (also in the background), keeps the applied / previous corrections, and
// saves / restores everything with the session.
//
// Verify captures are measured through the EQ. They never feed the fit; the
// ones taken with the current correction are averaged separately.
class MeasurementEngine : public juce::ChangeBroadcaster,
                          private juce::Timer
{
public:
    struct Entry
    {
        int id = 0;
        std::shared_ptr<const roomeq::Capture> capture;
        bool verify = false;
        int correctionId = 0;     // verify captures: the applied correction they measured
        roomeq::Arrival arrival;  // the direct sound's loop delay, found when it was analysed
    };

    struct SweepSettings
    {
        double seconds = 5.0;
        int repeats = 2;
        int channel = 0;          // speaker the sweep plays on: 0 = left, 1 = right
        double levelDbfs = -12.0;
    };

    // What the fit works to; polled from the processor.
    struct CorrectionSettings
    {
        roomeq::TargetCurve target = roomeq::flatTarget();
        roomeq::CorrectionConfig config;
        double fs = 48000.0;
    };

    // Computed in the background from a snapshot, so ids travel with it.
    struct Display
    {
        roomeq::SessionSummary summary;              // fit captures only
        std::vector<int> ids;                        // per position curve
        std::vector<bool> excluded;
        std::vector<double> targetDb;                // anchored target on the display grid
        std::optional<roomeq::CorrectionResult> proposal;
        double fitFs = 48000.0;
        std::vector<double> verifiedDb;              // verify average, aligned to the fit average (empty if none)
        int verifiedCount = 0;
        std::vector<int> verifyIds;                  // every verify capture, for highlighting one...
        std::vector<std::vector<double>> verifyDb;   // ...its curve, aligned to the fit average
    };

    enum class Activity
    {
        idle,
        measuring,
        analysing
    };

    explicit MeasurementEngine (CaptureRecorder& recorderToUse);
    ~MeasurementEngine() override;

    // Measurements. replaceId redoes an existing capture (keeping its name).
    // verify plays through the EQ and files the capture as a verify capture.
    juce::Result startSweep (double sampleRate, const SweepSettings& settings, int replaceId = -1, bool verify = false);
    juce::Result startNoise (double sampleRate, double seconds, int channel, double levelDbfs, int replaceId = -1,
                             bool verify = false);
    // Music plays on while the correction, voicing and loudness glide out; the
    // recording starts this long after.
    static constexpr double programSettleSeconds = 1.5;
    juce::Result startProgram (double sampleRate, double seconds, int replaceId = -1);
    // Loudness calibration: pink noise on both speakers through the EQ. Nothing is
    // recorded or filed here (the loudness stage's tap records what it needs).
    juce::Result startCalibration (double sampleRate, double seconds, double levelDbfs);
    // System latency: a sweep through a cable from the output to the mic input.
    // Its arrival is the interface and host round trip, taken off arrivals to
    // give flight times. Nothing is filed as a capture.
    juce::Result startLatencyMeasurement (double sampleRate, const SweepSettings& settings);
    std::optional<double> getSystemLatencyMs() const { return systemLatencyMs; }
    void setSystemLatencyMs (std::optional<double> ms);   // typed in, or cleared
    void cancel();

    Activity getActivity() const;
    float getProgress() const;
    juce::String getStatus() const;

    // Captures.
    std::vector<Entry> getEntries() const;
    void rename (int id, const juce::String& newName);
    void setExcluded (int id, bool excluded);
    void remove (int id);
    // Every capture and both corrections (applied and previous): the room starts
    // over. Only while nothing is measuring or analysing.
    void clearAll();

    void setSmoothingFraction (int fraction);
    void setSmoothingSource (std::function<int()> source) { smoothingSource = std::move (source); }
    void setCorrectionSettingsSource (std::function<CorrectionSettings()> source) { settingsSource = std::move (source); }
    std::shared_ptr<const Display> getDisplay() const;
    bool isDisplayCurrent() const;   // no analysis pending and the display reflects the latest change

    // Correction. Apply keeps the one it replaces as "previous".
    bool canApply() const;                       // a proposal exists and differs from the applied one
    void applyProposal();
    void undoApply();                            // swaps applied and previous
    void setComparingPrevious (bool shouldCompare);
    bool isComparingPrevious() const { return comparing; }
    bool hasPrevious() const { return hasPreviousCorrection; }
    const std::vector<roomeq::Band>& getApplied() const { return applied; }
    const std::vector<roomeq::Band>& getPrevious() const { return previous; }
    int getAppliedId() const { return appliedId; }
    const std::vector<roomeq::Band>& getPlaying() const { return comparing ? previous : applied; }

    // Called on the message thread whenever the correction to play changes.
    std::function<void (const std::vector<roomeq::Band>&)> onPlayingCorrectionChanged;

    // Persistence (safe from any thread).
    juce::ValueTree toValueTree() const;
    void fromValueTree (const juce::ValueTree& tree);

    // Collects finished recordings and background results. Driven by a timer;
    // public so tests can pump it.
    void update();

    static inline const juce::Identifier treeType { "Measurements" };

private:
    struct AnalysisResult
    {
        int replaceId = -1;
        bool verify = false;
        int correctionId = 0;
        std::shared_ptr<const roomeq::Capture> capture;
        roomeq::GradingConfig grading;    // what it was graded with
        roomeq::Arrival arrival;
        bool latency = false;             // a loopback for the system latency, not a capture
        juce::String name, error;
    };

    // Shared with background jobs so they can finish safely after we're gone.
    struct Mailbox
    {
        std::mutex lock;
        std::vector<AnalysisResult> results;
        std::shared_ptr<const Display> display;
        int summaryGeneration = 0;
        bool summaryReady = false;
        std::atomic<int> latestRequested { 0 };   // queued summaries older than this are skipped
    };

    void timerCallback() override { update(); }
    juce::Result startRequest (std::unique_ptr<CaptureRequest> request, int replaceId, bool verify);
    void analyse (std::unique_ptr<CaptureRequest> request);
    void requestSummary();
    void regradeAll();   // every capture against the current reference band
    void replaceCapture (int id, const std::function<void (roomeq::Capture&)>& change);
    void playingChanged();

    CaptureRecorder& recorder;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    juce::ThreadPool pool { juce::ThreadPoolOptions {}.withThreadName ("Room analysis").withNumberOfThreads (1) };

    mutable std::mutex stateLock;   // guards entries and display for readers on other threads
    std::vector<Entry> entries;
    std::shared_ptr<const Display> display;

    int nextId = 1;
    int nextNumber = 1;
    int nextVerifyNumber = 1;
    int smoothingFraction = 6;
    std::function<int()> smoothingSource;
    std::function<CorrectionSettings()> settingsSource;
    std::optional<CorrectionSettings> lastSettings;
    int summaryGeneration = 0;
    int displayGeneration = 0;
    int analysesPending = 0;
    juce::String pendingName, status;
    bool pendingVerify = false;
    bool pendingLatency = false;
    std::optional<double> systemLatencyMs;
    int pendingCorrectionId = 0;
    const std::vector<double> grid = roomeq::logFreqGrid (20.0, 20000.0, 48);

    std::vector<roomeq::Band> applied, previous;
    bool hasPreviousCorrection = false;
    bool comparing = false;
    int appliedId = 0;        // 0 = no correction applied yet
    int previousId = 0;
    int nextCorrectionId = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasurementEngine)
};
