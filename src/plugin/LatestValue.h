#pragma once

#include <atomic>

// Single-writer / single-reader "latest value" hand-off (triple buffer).
// The writer never waits for the reader and the reader never sees a torn
// value; intermediate values the reader missed are simply skipped.
// T must be trivially copyable. Lock-free and allocation-free.
template <typename T>
class LatestValue
{
public:
    // Writer thread.
    void write (const T& value) noexcept
    {
        buffers[back] = value;
        back = state.exchange (back | dirtyBit, std::memory_order_acq_rel) & indexMask;
    }

    // Reader thread: true (and `out` updated) if a new value arrived since the last read.
    bool read (T& out) noexcept
    {
        if ((state.load (std::memory_order_acquire) & dirtyBit) == 0)
            return false;
        front = state.exchange (front, std::memory_order_acq_rel) & indexMask;
        out = buffers[front];
        return true;
    }

private:
    static constexpr int dirtyBit = 4;
    static constexpr int indexMask = 3;

    T buffers[3] {};
    std::atomic<int> state { 1 };   // index of the middle buffer, plus dirtyBit
    int back = 0;                   // writer's
    int front = 2;                  // reader's
};
