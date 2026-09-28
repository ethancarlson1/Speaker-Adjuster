#pragma once

#include "roomeq/sweep.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One measurement in flight: what to play and where to record. Built (and all
// buffers allocated) on the message thread, filled by the audio thread, then
// handed back for analysis.
struct CaptureRequest
{
    enum class Kind
    {
        sweep,     // play the sweep on one speaker, record the mic
        program    // pass program through, record it and the mic (dual-FFT)
    };

    Kind kind = Kind::sweep;
    double sampleRate = 0.0;
    int sweepChannel = 0;                     // 0 = left, 1 = right
    int repeats = 1;
    roomeq::SweepConfig sweepConfig;
    std::vector<float> excitation;            // one repeat: preroll + sweep + tail
    std::vector<std::vector<float>> mic;      // per repeat (sweep) or a single take (program)
    std::vector<float> reference;             // program only: mono sum of the main input

    int replaceId = -1;                       // capture being redone, or -1 for a new one

    std::int64_t totalSamples = 0;
    std::atomic<std::int64_t> samplesDone { 0 };
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> finished { false };
    bool cancelled = false;                   // valid once finished

    // Audio-thread cursor.
    int repeat = 0;
    std::size_t position = 0;
};

// Plays sweeps and records the measurement mic on the audio thread.
//
// Hand-off: the message thread owns the request and publishes a raw pointer
// through `active`. The audio thread clears `active` before setting
// `finished`, and never touches the request afterwards, so once `finished` is
// true the message thread may take it back. Nothing on the audio thread
// allocates, locks or waits.
class CaptureRecorder
{
public:
    // Message thread.
    bool start (std::unique_ptr<CaptureRequest> request);
    void cancel();
    bool isBusy() const { return owned != nullptr; }
    std::unique_ptr<CaptureRequest> collectFinished();
    float getProgress() const;
    const CaptureRequest* getCurrent() const { return owned.get(); }

    // Only while audio is stopped (prepareToPlay / releaseResources).
    void abortWhileStopped();

    // Audio thread. `main` is the stereo main bus, processed in place (it holds
    // the input on entry). `mic` may be null when no mic is connected.
    void process (float* const* main, int numMainChannels, const float* mic, int numSamples) noexcept;

private:
    static void finish (CaptureRequest& r, bool cancelled, std::atomic<CaptureRequest*>& active) noexcept;

    std::unique_ptr<CaptureRequest> owned;
    std::atomic<CaptureRequest*> active { nullptr };
};

// Builds a sweep request with buffers sized for `repeats` takes.
std::unique_ptr<CaptureRequest> makeSweepRequest (double sampleRate, double seconds, int repeats,
                                                  int channel, double levelDbfs);

// Builds a program-material request recording `seconds` of input and mic.
std::unique_ptr<CaptureRequest> makeProgramRequest (double sampleRate, double seconds);
