#include "plugin/CaptureRecorder.h"

#include "roomeq/noise.h"

#include <algorithm>

bool CaptureRecorder::start (std::unique_ptr<CaptureRequest> request)
{
    if (owned != nullptr || request == nullptr)
        return false;
    owned = std::move (request);
    active.store (owned.get(), std::memory_order_release);
    return true;
}

void CaptureRecorder::cancel()
{
    if (owned != nullptr)
        owned->cancelRequested.store (true, std::memory_order_relaxed);
}

std::unique_ptr<CaptureRequest> CaptureRecorder::collectFinished()
{
    if (owned != nullptr && owned->finished.load (std::memory_order_acquire))
        return std::move (owned);
    return nullptr;
}

float CaptureRecorder::getProgress() const
{
    if (owned == nullptr || owned->totalSamples <= 0)
        return 0.0f;
    return static_cast<float> (owned->samplesDone.load (std::memory_order_relaxed))
           / static_cast<float> (owned->totalSamples);
}

void CaptureRecorder::abortWhileStopped()
{
    if (owned != nullptr && ! owned->finished.load())
    {
        active.store (nullptr);
        owned->cancelled = true;
        owned->finished.store (true);
    }
}

void CaptureRecorder::finish (CaptureRequest& r, bool cancelled, std::atomic<CaptureRequest*>& active) noexcept
{
    r.cancelled = cancelled;
    active.store (nullptr, std::memory_order_release);
    r.finished.store (true, std::memory_order_release);   // last access by the audio thread
}

bool CaptureRecorder::process (float* const* main, int numMainChannels, const float* mic, int numSamples,
                               bool eqFollows) noexcept
{
    auto* r = active.load (std::memory_order_acquire);
    if (r == nullptr)
        return false;
    const auto notStarted = r->repeat == 0 && r->position == 0;
    if (r->throughEq != eqFollows && notStarted && ! r->cancelRequested.load (std::memory_order_relaxed))
        return false;

    const auto n = static_cast<std::size_t> (numSamples);

    if (r->cancelRequested.load (std::memory_order_relaxed))
    {
        const auto wasPlaying = r->kind != CaptureRequest::Kind::program;
        if (wasPlaying)
            for (int ch = 0; ch < numMainChannels; ++ch)
                std::fill (main[ch], main[ch] + n, 0.0f);
        finish (*r, true, active);
        return wasPlaying;
    }

    if (r->kind != CaptureRequest::Kind::program)
    {
        // The sweep (or noise) replaces the program: it plays on one speaker, the other is silent.
        const auto sweepChannel = std::min (r->sweepChannel, numMainChannels - 1);
        const auto takeLength = r->excitation.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            auto out = 0.0f;
            if (r->repeat < r->repeats)
            {
                out = r->excitation[r->position];
                r->mic[static_cast<std::size_t> (r->repeat)][r->position] = mic != nullptr ? mic[i] : 0.0f;
                if (++r->position == takeLength)
                {
                    r->position = 0;
                    ++r->repeat;
                }
            }
            for (int ch = 0; ch < numMainChannels; ++ch)
                main[ch][i] = ch == sweepChannel ? out : 0.0f;
        }
        r->samplesDone.store (static_cast<std::int64_t> (static_cast<std::size_t> (r->repeat) * takeLength + r->position),
                              std::memory_order_relaxed);
        if (r->repeat >= r->repeats)
            finish (*r, false, active);
        return true;
    }

    // Program: audio passes through untouched; record the input and the mic.
    const auto remaining = r->reference.size() - r->position;
    const auto count = std::min (n, remaining);
    const auto gain = numMainChannels > 1 ? 0.5f : 1.0f;
    for (std::size_t i = 0; i < count; ++i)
    {
        auto sum = 0.0f;
        for (int ch = 0; ch < std::min (numMainChannels, 2); ++ch)
            sum += main[ch][i];
        r->reference[r->position + i] = gain * sum;
        r->mic[0][r->position + i] = mic != nullptr ? mic[i] : 0.0f;
    }
    r->position += count;
    r->samplesDone.store (static_cast<std::int64_t> (r->position), std::memory_order_relaxed);
    if (r->position == r->reference.size())
        finish (*r, false, active);
    return false;
}

std::unique_ptr<CaptureRequest> makeSweepRequest (double sampleRate, double seconds, int repeats,
                                                  int channel, double levelDbfs)
{
    auto r = std::make_unique<CaptureRequest>();
    r->kind = CaptureRequest::Kind::sweep;
    r->sampleRate = sampleRate;
    r->sweepChannel = channel;
    r->repeats = std::max (1, repeats);
    r->sweepConfig.fs = sampleRate;
    r->sweepConfig.duration = seconds;
    r->sweepConfig.levelDbfs = levelDbfs;

    const auto playback = roomeq::buildPlayback (r->sweepConfig, roomeq::generateSweep (r->sweepConfig));
    r->excitation.assign (playback.begin(), playback.end());
    r->mic.assign (static_cast<std::size_t> (r->repeats), std::vector<float> (r->excitation.size(), 0.0f));
    r->totalSamples = static_cast<std::int64_t> (r->excitation.size()) * r->repeats;
    return r;
}

std::unique_ptr<CaptureRequest> makeNoiseRequest (double sampleRate, double seconds, int channel, double levelDbfs)
{
    roomeq::NoiseConfig cfg;
    cfg.fs = sampleRate;
    cfg.duration = seconds;
    cfg.levelDbfs = levelDbfs;
    const auto noise = roomeq::generatePinkNoise (cfg);

    auto r = std::make_unique<CaptureRequest>();
    r->kind = CaptureRequest::Kind::noise;
    r->sampleRate = sampleRate;
    r->sweepChannel = channel;
    r->repeats = 1;
    r->reference.assign (noise.begin(), noise.end());
    r->excitation = r->reference;
    r->excitation.resize (noise.size() + cfg.nTail(), 0.0f);   // let the room decay
    r->mic.assign (1, std::vector<float> (r->excitation.size(), 0.0f));
    r->totalSamples = static_cast<std::int64_t> (r->excitation.size());
    return r;
}

std::unique_ptr<CaptureRequest> makeProgramRequest (double sampleRate, double seconds)
{
    auto r = std::make_unique<CaptureRequest>();
    r->kind = CaptureRequest::Kind::program;
    r->sampleRate = sampleRate;
    r->sweepConfig.fs = sampleRate;
    const auto length = static_cast<std::size_t> (seconds * sampleRate);
    r->reference.assign (length, 0.0f);
    r->mic.assign (1, std::vector<float> (length, 0.0f));
    r->totalSamples = static_cast<std::int64_t> (length);
    return r;
}
