#include "plugin/SplMeter.h"

#include "roomeq/loudness.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double silentMs = 1e-10;   // -100 dBFS: nothing from the mic

float toDb (double ms)
{
    return ms > 0.0 ? static_cast<float> (10.0 * std::log10 (ms)) : SplMeter::noneDb;
}
} // namespace

void SplMeter::prepare (double sampleRate)
{
    const auto design = [sampleRate] (const auto& bands, auto& sections)
    {
        for (std::size_t i = 0; i < sections.size(); ++i)
        {
            const auto c = roomeq::designBiquad (bands[i], sampleRate);
            sections[i] = { c.b0, c.b1, c.b2, c.a1, c.a2, 0.0, 0.0 };
        }
    };
    design (roomeq::aWeightingBands(), weightA);
    design (roomeq::cWeightingBands(), weightC);
    gainA = roomeq::aWeightingGain (sampleRate);
    gainC = roomeq::cWeightingGain (sampleRate);
    fastCoeff = 1.0 - std::exp (-1.0 / (fastSeconds * sampleRate));
    fastMs = accA = accC = 0.0;
    samplesPerSecond = std::max (1L, std::lround (sampleRate));
    inSecond = 0;
    fast.store (noneDb);
}

void SplMeter::process (const float* mic, int numSamples) noexcept
{
    if (mic == nullptr)
        return;
    for (int n = 0; n < numSamples; ++n)
    {
        const auto x = static_cast<double> (mic[n]);
        auto a = gainA * x;
        for (auto& s : weightA)
            a = s.tick (a);
        auto c = gainC * x;
        for (auto& s : weightC)
            c = s.tick (c);
        fastMs += fastCoeff * (a * a - fastMs);
        accA += a * a;
        accC += c * c;
        if (++inSecond == samplesPerSecond)
        {
            const auto w = written.load (std::memory_order_relaxed);
            ring[w % ring.size()] = { accA / static_cast<double> (inSecond), accC / static_cast<double> (inSecond) };
            written.store (w + 1, std::memory_order_release);
            accA = accC = 0.0;
            inSecond = 0;
        }
    }
    const auto now = toDb (fastMs);
    fast.store (now, std::memory_order_relaxed);
    auto p = peak.load (std::memory_order_relaxed);
    while (now > p && ! peak.compare_exchange_weak (p, now, std::memory_order_relaxed))
    {
    }
}

void SplMeter::collect()
{
    const auto w = written.load (std::memory_order_acquire);
    if (w - read > ring.size() - 8)   // fell behind: the oldest may be being overwritten
        read = w - static_cast<std::uint32_t> (ring.size() - 8);
    for (; read != w; ++read)
    {
        const auto s = ring[read % ring.size()];
        if (s.c <= silentMs)
            continue;
        window[static_cast<std::size_t> (head)] = s;
        head = (head + 1) % leqSeconds;
        count = std::min (count + 1, leqSeconds);
    }
    const auto p = peak.exchange (noneDb, std::memory_order_relaxed);
    if (p > toDb (silentMs))
        maxFast = std::max (maxFast, p);
}

void SplMeter::reset()
{
    collect();   // what's in flight belongs to before the reset
    head = count = 0;
    maxFast = noneDb;
}

double SplMeter::leqDb (int which) const noexcept
{
    if (count == 0)
        return noneDb;
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto& s = window[static_cast<std::size_t> ((head - 1 - i + leqSeconds) % leqSeconds)];
        sum += which == 0 ? s.a : s.c;
    }
    return 10.0 * std::log10 (sum / count);
}
