#pragma once

#include "plugin/TapRecorder.h"
#include "plugin/WarningDismissal.h"
#include "roomeq/showtrack.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Show mode, message-thread side (Phase 4, delta tracking):
//
// - Store reference: ~30 s of whatever plays at the end of soundcheck (music or
//   pink noise from the desk), analysed into the output-to-mic response per
//   third octave. Saved with the session.
// - While a reference exists, the show is measured from the music in 10 s
//   blocks back to back (a tap after all the EQ, so our own EQ changes aren't
//   mistaken for the room's), and a roomeq::DeltaTracker compares the last
//   2 minutes with the reference.
// - Blocks that overlap a measurement or calibration (a test signal instead
//   of the show), or during which the mic went dead, are dropped.
//
// Analysis runs on a background thread; nothing here blocks the audio thread.
class ShowController : public juce::ChangeBroadcaster,
                       private juce::Timer
{
public:
    struct Environment
    {
        double fs = 0.0;                 // 0 while audio isn't running
        bool micSignal = false;          // the mic is connected and carried signal in the last second
        bool measuring = false;          // a test signal is playing (measurement or calibration)
        bool available = true;           // false in the standalone app
    };

    enum class Step
    {
        idle,
        storing,       // recording the reference
        tracking       // recording a show block
    };

    struct Info
    {
        bool hasReference = false;
        juce::int64 storedAt = 0;        // ms since the epoch
        int referenceBands = 0;          // bands the mic heard clearly in the reference
        roomeq::ShowState state;
        int blocksHeard = 0;             // show blocks analysed since the reference
        int blocksDropped = 0;           // skipped: a measurement played, or the mic was dead
        bool warningDismissed = false;   // the banner's ×: nothing new since
        // Countdown. While a show block records: seconds until it's in (the next
        // update), and until the first result if there isn't one yet (an estimate:
        // blocks the mic can't hear clearly don't count). -1 when not recording.
        double nextUpdateSeconds = -1.0;
        double firstResultSeconds = -1.0;
        juce::String paused;             // why no block is recording ("" when one is, or there's no reference)
    };

    static constexpr double referenceSeconds = 30.0;
    static constexpr double blockSeconds = 10.0;

    explicit ShowController (TapRecorder& tapToUse);
    ~ShowController() override;

    void setEnvironmentSource (std::function<Environment()> source) { environmentSource = std::move (source); }

    juce::Result storeReference();
    void cancelStore();                  // stops a reference being stored (the old one stays)
    void clearReference();
    void dismissWarning();               // hides the current warning until something new (see WarningDismissal)

    Step getStep() const { return step; }
    float getProgress() const;
    juce::String getStatus() const { return status; }
    Info getInfo() const;
    int pendingAnalyses() const { return pending; }   // for tests: wait until it's 0

    // Persistence (safe from any thread).
    juce::ValueTree toValueTree() const;
    void fromValueTree (const juce::ValueTree& tree);

    // Driven by a timer; public so tests can pump it.
    void update();

    static inline const juce::Identifier treeType { "Show" };

private:
    struct Result
    {
        bool reference = false;
        int generation = 0;              // results from before a new or cleared reference are ignored
        std::vector<double> bands;
    };

    // Shared with background jobs so they can finish safely after we're gone.
    struct Mailbox
    {
        std::mutex lock;
        std::vector<Result> results;
    };

    void timerCallback() override { update(); }
    Environment environment() const;
    bool startTap (Step newStep, double seconds);
    void analyse (std::unique_ptr<TapRequest> tap, bool reference);
    void handle (const Result& r);

    TapRecorder& tap;
    std::function<Environment()> environmentSource;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    juce::ThreadPool pool { juce::ThreadPoolOptions {}.withThreadName ("Show tracking").withNumberOfThreads (1) };

    Step step = Step::idle;
    bool storeRequested = false;         // waiting for a tracking block to stop first
    bool blockSpoiled = false;           // the block in flight overlapped a measurement or a dead mic
    double tapFs = 48000.0;
    int pending = 0;
    int generation = 0;
    juce::String status;

    // Guarded for readers on other threads (state save).
    mutable std::mutex stateLock;
    std::vector<double> reference;
    juce::int64 storedAt = 0;
    std::optional<roomeq::DeltaTracker> tracker;
    int blocksHeard = 0, blocksDropped = 0;
    WarningDismissal dismissal;          // not saved: a new session shows what it finds

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShowController)
};

// "Next update in 7 s.", "First result in about 0:45.", or why it's paused ("" with no reference).
juce::String showCountdown (const ShowController::Info& info);
