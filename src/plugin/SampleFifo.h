#pragma once

// One writer (the audio thread) pushes samples, one reader (the UI) pulls
// what's new since it last looked. The writer never waits: if the reader
// falls more than half the ring behind, it skips to the newest samples.
// JUCE-free, so it's unit tested.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class SampleFifo
{
public:
    explicit SampleFifo (std::size_t capacity = 1 << 16) : ring (capacity, 0.0f) {}

    // Audio thread.
    void push (const float* x, int numSamples) noexcept
    {
        auto w = written.load (std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i)
            ring[static_cast<std::size_t> (w++ % ring.size())] = x[i];
        written.store (w, std::memory_order_release);
    }

    // Reader: copies up to `max` of the newest unread samples into dest; returns how many.
    // Samples it had fallen too far behind to read are skipped.
    int pull (float* dest, int max)
    {
        const auto w = written.load (std::memory_order_acquire);
        const auto safe = static_cast<std::uint64_t> (ring.size() / 2);
        if (w - read > safe)
            read = w - safe;
        const auto n = static_cast<int> (std::min<std::uint64_t> (w - read, static_cast<std::uint64_t> (std::max (0, max))));
        for (int i = 0; i < n; ++i)
            dest[i] = ring[static_cast<std::size_t> (read++ % ring.size())];
        return n;
    }

    std::uint64_t totalPushed() const noexcept { return written.load (std::memory_order_acquire); }

private:
    std::vector<float> ring;
    std::atomic<std::uint64_t> written { 0 };
    std::uint64_t read = 0;
};
