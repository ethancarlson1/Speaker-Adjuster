#pragma once

// Records the final output (mono) and the mic for a set time, for analysis on
// the message thread: the loudness calibration, re-check and mic calibrator,
// and show tracking. JUCE-free, so it's unit tested.
//
// Hand-off like CaptureRecorder's requests: the message thread owns the
// request and publishes a raw pointer; the audio thread clears it before
// setting `finished` and never touches the request afterwards. The audio
// thread never allocates, locks or waits.

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

struct TapRequest
{
    std::vector<float> output, mic;   // sized by the message thread (mic may be empty)
    std::size_t skip = 0;             // samples to let pass before recording
    std::size_t position = 0;         // audio-thread cursor (skip counted first)
    std::atomic<bool> cancelRequested { false };
    std::atomic<bool> finished { false };
    bool cancelled = false;           // valid once finished
};

class TapRecorder
{
public:
    // Message thread.
    bool start (std::unique_ptr<TapRequest> request);
    void cancel();
    bool isBusy() const { return tap != nullptr; }
    std::unique_ptr<TapRequest> collect();
    float getProgress() const;

    // Only while audio is stopped (prepareToPlay / releaseResources).
    void abortWhileStopped();

    // Audio thread: `channels` is the final output; `mic` may be null.
    void record (const float* const* channels, int numChannels, const float* mic, int numSamples) noexcept;

private:
    std::unique_ptr<TapRequest> tap;                // owned by the message thread
    std::atomic<TapRequest*> active { nullptr };    // written by the audio thread while set
};
