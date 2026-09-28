#pragma once

#include "plugin/CaptureRecorder.h"
#include "roomeq/averaging.h"
#include "roomeq/capture.h"

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Message-thread side of Phase 1: starts measurements, hands finished
// recordings to a background thread for analysis, keeps the capture list,
// recomputes the averaged response (also in the background), and saves /
// restores everything with the session.
class MeasurementEngine : public juce::ChangeBroadcaster,
                          private juce::Timer
{
public:
    struct Entry
    {
        int id = 0;
        std::shared_ptr<const roomeq::Capture> capture;
    };

    struct SweepSettings
    {
        double seconds = 5.0;
        int repeats = 2;
        int channel = 0;          // speaker the sweep plays on: 0 = left, 1 = right
        double levelDbfs = -12.0;
    };

    // The averaged response plus which capture each position curve belongs to
    // (computed in the background from a snapshot, so ids travel with it).
    struct Display
    {
        roomeq::SessionSummary summary;
        std::vector<int> ids;
        std::vector<bool> excluded;
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
    juce::Result startSweep (double sampleRate, const SweepSettings& settings, int replaceId = -1);
    juce::Result startNoise (double sampleRate, double seconds, int channel, double levelDbfs, int replaceId = -1);
    juce::Result startProgram (double sampleRate, double seconds, int replaceId = -1);
    void cancel();

    Activity getActivity() const;
    float getProgress() const;
    juce::String getStatus() const;

    // Captures.
    std::vector<Entry> getEntries() const;
    void rename (int id, const juce::String& newName);
    void setExcluded (int id, bool excluded);
    void remove (int id);

    void setSmoothingFraction (int fraction);
    void setSmoothingSource (std::function<int()> source) { smoothingSource = std::move (source); }
    std::shared_ptr<const Display> getDisplay() const;

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
        std::shared_ptr<const roomeq::Capture> capture;
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
    };

    void timerCallback() override { update(); }
    juce::Result startRequest (std::unique_ptr<CaptureRequest> request, int replaceId);
    void analyse (std::unique_ptr<CaptureRequest> request);
    void requestSummary();
    void replaceCapture (int id, const std::function<void (roomeq::Capture&)>& change);

    CaptureRecorder& recorder;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    juce::ThreadPool pool { juce::ThreadPoolOptions {}.withThreadName ("Room analysis").withNumberOfThreads (1) };

    mutable std::mutex stateLock;   // guards entries and display for readers on other threads
    std::vector<Entry> entries;
    std::shared_ptr<const Display> display;

    int nextId = 1;
    int nextNumber = 1;
    int smoothingFraction = 6;
    std::function<int()> smoothingSource;
    int summaryGeneration = 0;
    int analysesPending = 0;
    juce::String pendingName, status;
    const std::vector<double> grid = roomeq::logFreqGrid (20.0, 20000.0, 48);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeasurementEngine)
};
