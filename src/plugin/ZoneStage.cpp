#include "plugin/ZoneStage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double kaiserBeta = 8.0;
constexpr int half = ZoneStage::taps / 2;   // the kernel spans half - 1 samples before its centre and half after

double besselI0 (double x) noexcept
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40 && term > 1e-12 * sum; ++k)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}
} // namespace

std::string zoneDelayText (double delayMs)
{
    const auto metres = delayMs / 1000.0 * speedOfSound;
    char text[64];
    std::snprintf (text, sizeof (text), "%.2f ms (%.2f m / %.1f ft)", delayMs, metres, metres / 0.3048);
    return text;
}

double parseZoneDelay (const std::string& text)
{
    // Parsed by hand: strtod follows the host's locale.
    std::size_t i = 0;
    const auto n = text.size();
    while (i < n && ! (std::isdigit (static_cast<unsigned char> (text[i])) || text[i] == '.' || text[i] == ','))
        ++i;
    double value = 0.0, scale = 0.0;
    bool any = false;
    for (; i < n; ++i)
    {
        const auto c = text[i];
        if (std::isdigit (static_cast<unsigned char> (c)))
        {
            any = true;
            if (scale > 0.0)
            {
                value += (c - '0') * scale;
                scale *= 0.1;
            }
            else
            {
                value = value * 10.0 + (c - '0');
            }
        }
        else if ((c == '.' || c == ',') && scale <= 0.0)
        {
            scale = 0.1;
        }
        else
        {
            break;
        }
    }
    if (! any)
        return 0.0;
    while (i < n && std::isspace (static_cast<unsigned char> (text[i])))
        ++i;
    const auto unit = [&] (const char* u)
    {
        for (std::size_t k = 0; u[k] != 0; ++k)
            if (i + k >= n || std::tolower (static_cast<unsigned char> (text[i + k])) != u[k])
                return false;
        return true;
    };
    auto metres = -1.0;
    if (unit ("ms"))
        metres = -1.0;
    else if (unit ("cm"))
        metres = value / 100.0;
    else if (unit ("ft") || unit ("'"))
        metres = value * 0.3048;
    else if (unit ("m"))
        metres = value;
    const auto ms = metres >= 0.0 ? metres / speedOfSound * 1000.0 : value;
    return std::clamp (ms, 0.0, ZoneStage::maxDelayMs);
}

double ZoneStage::delaySamples (double delayMs, double sampleRate) noexcept
{
    return std::clamp (delayMs, 0.0, maxDelayMs) * sampleRate / 1000.0;
}

void ZoneStage::prepare (double sampleRate, const ZoneSettings& initial)
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto needed = static_cast<int> (std::ceil (delaySamples (maxDelayMs, fs))) + taps + 1;
    auto size = 1;
    while (size < needed)
        size <<= 1;
    for (auto& line : lines)
        line.assign (static_cast<std::size_t> (size), 0.0f);
    mask = size - 1;
    writePos = 0;
    fadeLength = std::max (1, static_cast<int> (std::lround (fadeSeconds * fs)));
    fadePos = fadeLength;
    current = next = makeTap (initial);
}

ZoneStage::Tap ZoneStage::makeTap (const ZoneSettings& s) const noexcept
{
    Tap t;
    t.settings = s;
    const auto sign = s.invert ? -1.0f : 1.0f;
    const auto d = delaySamples (s.delayMs, fs);
    const auto whole = std::floor (d);
    const auto frac = d - whole;
    if (whole < half - 1 || frac < 1e-6 || frac > 1.0 - 1e-6)
    {
        // Short delays (no room for the kernel without looking ahead) and whole samples.
        t.whole = static_cast<int> (std::lround (d));
        t.gain = sign;
        return t;
    }
    t.fractional = true;
    t.whole = static_cast<int> (whole);
    double sum = 0.0;
    std::array<double, taps> h {};
    const auto norm = besselI0 (kaiserBeta);
    for (int j = 0; j < taps; ++j)
    {
        const auto x = static_cast<double> (j - (half - 1)) - frac;   // this tap's distance from the exact delay
        const auto r = x / half;
        const auto window = besselI0 (kaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / norm;
        const auto sinc = std::abs (x) < 1e-12 ? 1.0 : std::sin (pi * x) / (pi * x);
        h[static_cast<std::size_t> (j)] = sinc * window;
        sum += h[static_cast<std::size_t> (j)];
    }
    for (int j = 0; j < taps; ++j)
        t.kernel[static_cast<std::size_t> (j)] = static_cast<float> (h[static_cast<std::size_t> (j)] / sum) * sign;
    return t;
}

float ZoneStage::read (const Tap& t, const std::vector<float>& line) const noexcept
{
    if (! t.fractional)
        return t.gain * line[static_cast<std::size_t> ((writePos - t.whole) & mask)];
    // Tap j reads the sample (whole - (half - 1) + j) back.
    auto pos = writePos - (t.whole - (half - 1));
    float y = 0.0f;
    for (int j = 0; j < taps; ++j, --pos)
        y += t.kernel[static_cast<std::size_t> (j)] * line[static_cast<std::size_t> (pos & mask)];
    return y;
}

void ZoneStage::process (float* const* channels, int numChannels, int numSamples, const ZoneSettings& target) noexcept
{
    numChannels = std::min (numChannels, maxChannels);
    if (numChannels <= 0 || mask == 0)
        return;
    const auto startFade = [this] (const ZoneSettings& s)
    {
        next = makeTap (s);
        fadePos = 0;
    };
    if (! isFading() && target != current.settings)
        startFade (target);

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            lines[static_cast<std::size_t> (ch)][static_cast<std::size_t> (writePos)] = channels[ch][i];

        if (isFading())
        {
            const auto t = (static_cast<float> (fadePos) + 0.5f) / static_cast<float> (fadeLength);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto& line = lines[static_cast<std::size_t> (ch)];
                channels[ch][i] = (1.0f - t) * read (current, line) + t * read (next, line);
            }
            if (++fadePos == fadeLength)
            {
                current = next;
                if (target != current.settings)
                    startFade (target);
            }
        }
        else if (current.fractional || current.whole != 0 || current.gain < 0.0f)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][i] = read (current, lines[static_cast<std::size_t> (ch)]);
        }
        writePos = (writePos + 1) & mask;
    }
}
