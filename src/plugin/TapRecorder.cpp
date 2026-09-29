#include "plugin/TapRecorder.h"

#include <algorithm>

bool TapRecorder::start (std::unique_ptr<TapRequest> request)
{
    if (tap != nullptr || request == nullptr)
        return false;
    tap = std::move (request);
    active.store (tap.get(), std::memory_order_release);
    return true;
}

void TapRecorder::cancel()
{
    if (tap != nullptr)
        tap->cancelRequested.store (true);
}

void TapRecorder::abortWhileStopped()
{
    active.store (nullptr);
    if (tap != nullptr && ! tap->finished.load())
    {
        tap->cancelled = true;
        tap->finished.store (true);
    }
}

std::unique_ptr<TapRequest> TapRecorder::collect()
{
    if (tap != nullptr && tap->finished.load (std::memory_order_acquire))
        return std::move (tap);
    return nullptr;
}

float TapRecorder::getProgress() const
{
    if (tap == nullptr || tap->output.empty())
        return 0.0f;
    const auto recorded = tap->position > tap->skip ? tap->position - tap->skip : 0;
    return static_cast<float> (recorded) / static_cast<float> (tap->output.size());
}

void TapRecorder::record (const float* const* channels, int numChannels, const float* mic, int numSamples) noexcept
{
    auto* t = active.load (std::memory_order_acquire);
    if (t == nullptr)
        return;
    const auto total = t->skip + t->output.size();
    if (t->cancelRequested.load (std::memory_order_relaxed))
    {
        t->cancelled = true;
        t->position = total;
    }
    const auto scale = 1.0f / static_cast<float> (std::max (1, std::min (numChannels, 2)));
    for (int i = 0; i < numSamples && t->position < total; ++i, ++t->position)
    {
        if (t->position < t->skip)
            continue;
        const auto k = t->position - t->skip;
        auto s = numChannels > 0 ? channels[0][i] : 0.0f;
        if (numChannels > 1)
            s += channels[1][i];
        t->output[k] = s * scale;
        if (k < t->mic.size())
            t->mic[k] = mic != nullptr ? mic[i] : 0.0f;
    }
    if (t->position >= total)
    {
        active.store (nullptr, std::memory_order_release);
        t->finished.store (true, std::memory_order_release);
    }
}
