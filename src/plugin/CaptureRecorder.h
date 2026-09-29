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
        noise,     // play pink noise on one speaker, record the mic (dual-FFT)
        program    // pass program through, record it and the mic (dual-FFT)
    };

    enum class Purpose
    {
        capture,       // becomes a capture in the list
        calibration    // loudness calibration noise: only played (the loudness stage's tap records)
    };

    Kind kind = Kind::sweep;
    Purpose purpose = Purpose::capture;
    double sampleRate = 0.0;
    int sweepChannel = 0;                     // 0 = left, 1 = right
    bool allChannels = false;                 // play on every output (calibration)
    int repeats = 1;
    roomeq::SweepConfig sweepConfig;
    std::vector<float> excitation;            // one take: preroll + sweep + tail, or noise + tail
    std::vector<std::vector<float>> mic;      // per repeat (sweep) or a single take (noise, program)
    std::vector<float> reference;             // noise: what was played; program: mono sum of the input
    std::size_t settle = 0;                   // program: samples passed through before recording starts

    int replaceId = -1;                       // capture being redone, or -1 for a new one
    bool throughEq = false;                   // verify: the excitation plays through the correction and voicing EQ

    std::int64_t totalSamples = 0;
    std::atomic<std::int64_t> samplesDone { 0 };
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> finished { false };
    bool cancelled = false;                   // valid once finished

    // Audio-thread cursor.
    int repeat = 0;
    std::size_t position = 0;
    std::size_t settled = 0;                  // program: of `settle`
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
    // `eqFollows` says whether the caller runs the EQ after this call; a
    // request that wants the other order waits for the next block to start.
    // Returns true if it wrote the output (a sweep, or silence while stopping one).
    bool process (float* const* main, int numMainChannels, const float* mic, int numSamples,
                  bool eqFollows = false) noexcept;

    // Audio thread: whether the measurement in flight plays through the EQ.
    bool playsThroughEq() const noexcept
    {
        const auto* r = active.load (std::memory_order_acquire);
        return r != nullptr && r->throughEq;
    }

    // Audio thread: whether a measurement is in flight (the EQ and loudness step aside).
    bool isActive() const noexcept { return active.load (std::memory_order_acquire) != nullptr; }

    // Audio thread: whether the measurement in flight replaces the program
    // with a test signal (anything but a music capture).
    bool replacesOutput() const noexcept
    {
        const auto* r = active.load (std::memory_order_acquire);
        return r != nullptr && r->kind != CaptureRequest::Kind::program;
    }

private:
    static void finish (CaptureRequest& r, bool cancelled, std::atomic<CaptureRequest*>& active) noexcept;

    std::unique_ptr<CaptureRequest> owned;
    std::atomic<CaptureRequest*> active { nullptr };
};

// Builds a sweep request with buffers sized for `repeats` takes.
std::unique_ptr<CaptureRequest> makeSweepRequest (double sampleRate, double seconds, int repeats,
                                                  int channel, double levelDbfs);

// Builds a pink-noise request: `seconds` of noise on one speaker, then a 1 s tail.
std::unique_ptr<CaptureRequest> makeNoiseRequest (double sampleRate, double seconds, int channel, double levelDbfs);

// Builds a program-material request recording `seconds` of input and mic,
// after letting `settleSeconds` pass (while the EQ and loudness glide out).
std::unique_ptr<CaptureRequest> makeProgramRequest (double sampleRate, double seconds, double settleSeconds = 0.0);
