#include "plugin/EqStages.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
constexpr double pi = 3.14159265358979323846;

bool same (double a, double b) noexcept { return std::equal_to<double>() (a, b); }

bool sameCoeffs (const SvfCoeffs& a, const SvfCoeffs& b) noexcept
{
    return same (a.g, b.g) && same (a.k, b.k) && same (a.m0, b.m0) && same (a.m1, b.m1) && same (a.m2, b.m2);
}

bool isPassThrough (const SvfCoeffs& c) noexcept
{
    return same (c.m0, 1.0) && same (c.m1, 0.0) && same (c.m2, 0.0);
}

bool close (double target, double current) noexcept
{
    return std::abs (target - current) <= 1e-7 * std::max (1.0, std::abs (target));
}
} // namespace

SvfCoeffs designSvf (const roomeq::Band& band, double fs) noexcept
{
    SvfCoeffs c;
    if (! band.enabled)
        return c;
    const auto f0 = std::min (band.freq, 0.49 * fs);
    const auto g = std::tan (pi * f0 / fs);
    const auto a = std::pow (10.0, band.gainDb / 40.0);
    c.g = g;
    c.k = 1.0 / band.q;
    switch (band.kind)
    {
        case roomeq::BandKind::bell:
            c.k = 1.0 / (band.q * a);
            c.m0 = 1.0; c.m1 = c.k * (a * a - 1.0); c.m2 = 0.0;
            break;
        case roomeq::BandKind::lowShelf:
            c.g = g / std::sqrt (a);
            c.m0 = 1.0; c.m1 = c.k * (a - 1.0); c.m2 = a * a - 1.0;
            break;
        case roomeq::BandKind::highShelf:
            c.g = g * std::sqrt (a);
            c.m0 = a * a; c.m1 = c.k * (1.0 - a) * a; c.m2 = 1.0 - a * a;
            break;
        case roomeq::BandKind::highPass:
            c.m0 = 1.0; c.m1 = -c.k; c.m2 = -1.0;
            break;
        case roomeq::BandKind::lowPass:
            c.m0 = 0.0; c.m1 = 0.0; c.m2 = 1.0;
            break;
    }
    return c;
}

// ---------------------------------------------------------------------------

void EqChain::prepare (double sampleRate, double glideSeconds) noexcept
{
    glide = 1.0 - std::exp (-1.0 / (glideSeconds * sampleRate));
    reset();
}

void EqChain::reset() noexcept
{
    for (auto& s : sections)
        for (int ch = 0; ch < maxChannels; ++ch)
            s.ic1[ch] = s.ic2[ch] = 0.0;
}

void EqChain::updateDerived (Section& s) noexcept
{
    s.a1 = 1.0 / (1.0 + s.cur.g * (s.cur.g + s.cur.k));
    s.a2 = s.cur.g * s.a1;
    s.a3 = s.cur.g * s.a2;
}

void EqChain::setTargets (const SvfCoeffs* coeffs, int count) noexcept
{
    for (int i = 0; i < maxSections; ++i)
    {
        auto& s = sections[static_cast<std::size_t> (i)];
        const auto t = i < count ? coeffs[i] : SvfCoeffs {};
        if (sameCoeffs (t, s.tgt) && (s.gliding || sameCoeffs (t, s.cur)))
            continue;
        if (s.passThrough && ! s.gliding)
        {
            // Coming out of pass-through: start from the new filter's own
            // frequency and damping with the mix at pass-through, so only the
            // mix glides (the state was cleared while it was skipped).
            s.cur.g = t.g;
            s.cur.k = t.k;
            updateDerived (s);
        }
        s.tgt = t;
        s.gliding = true;
        s.passThrough = false;
    }
    updateActiveCount();
}

void EqChain::jumpToTargets() noexcept
{
    for (auto& s : sections)
    {
        s.cur = s.tgt;
        s.gliding = false;
        s.passThrough = isPassThrough (s.cur);
        updateDerived (s);
    }
    updateActiveCount();
}

bool EqChain::isGliding() const noexcept
{
    return std::any_of (sections.begin(), sections.end(), [] (const Section& s) { return s.gliding; });
}

void EqChain::updateActiveCount() noexcept
{
    numActive = 0;
    for (int i = 0; i < maxSections; ++i)
        if (! sections[static_cast<std::size_t> (i)].passThrough)
            numActive = i + 1;
}

void EqChain::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    numChannels = std::min (numChannels, maxChannels);
    if (numActive == 0)
        return;
    auto anyGliding = isGliding();
    for (int n = 0; n < numSamples; ++n)
    {
        if (anyGliding)
        {
            anyGliding = false;
            for (int i = 0; i < numActive; ++i)
            {
                auto& s = sections[static_cast<std::size_t> (i)];
                if (! s.gliding)
                    continue;
                auto& c = s.cur;
                const auto& t = s.tgt;
                c.g += glide * (t.g - c.g);
                c.k += glide * (t.k - c.k);
                c.m0 += glide * (t.m0 - c.m0);
                c.m1 += glide * (t.m1 - c.m1);
                c.m2 += glide * (t.m2 - c.m2);
                if (close (t.g, c.g) && close (t.k, c.k) && close (t.m0, c.m0) && close (t.m1, c.m1)
                    && close (t.m2, c.m2))
                {
                    c = t;
                    s.gliding = false;
                    if (isPassThrough (c))
                    {
                        s.passThrough = true;
                        for (int ch = 0; ch < maxChannels; ++ch)
                            s.ic1[ch] = s.ic2[ch] = 0.0;
                    }
                }
                else
                {
                    anyGliding = true;
                }
                updateDerived (s);
            }
        }
        for (int ch = 0; ch < numChannels; ++ch)
        {
            double x = channels[ch][n];
            for (int i = 0; i < numActive; ++i)
            {
                auto& s = sections[static_cast<std::size_t> (i)];
                if (s.passThrough)
                    continue;
                const auto v3 = x - s.ic2[ch];
                const auto v1 = s.a1 * s.ic1[ch] + s.a2 * v3;
                const auto v2 = s.ic2[ch] + s.a2 * s.ic1[ch] + s.a3 * v3;
                s.ic1[ch] = 2.0 * v1 - s.ic1[ch];
                s.ic2[ch] = 2.0 * v2 - s.ic2[ch];
                x = s.cur.m0 * x + s.cur.m1 * v1 + s.cur.m2 * v2;
            }
            channels[ch][n] = static_cast<float> (x);
        }
    }
    if (! anyGliding)
        updateActiveCount();
}

// ---------------------------------------------------------------------------

void EqStages::prepare (double sampleRate, const EqSettings& settings) noexcept
{
    fs = sampleRate;
    current = settings;
    pending.read (correction);
    correctionChain.prepare (fs);
    voicingChain.prepare (fs);
    updateCorrectionTargets();
    updateVoicingTargets();
    correctionChain.jumpToTargets();
    voicingChain.jumpToTargets();
}

void EqStages::updateCorrectionTargets() noexcept
{
    std::array<SvfCoeffs, EqChain::maxSections> c {};
    int n = 0;
    if (current.correctionOn)
        for (int i = 0; i < correction.count && n < EqChain::maxSections; ++i)
            c[static_cast<std::size_t> (n++)] = designSvf (correction.bands[static_cast<std::size_t> (i)].scaled (current.amount), fs);
    correctionChain.setTargets (c.data(), n);
}

void EqStages::updateVoicingTargets() noexcept
{
    std::array<SvfCoeffs, EqChain::maxSections> c {};
    int n = 0;
    if (current.voicingOn)
        for (const auto& v : current.voicing)
        {
            const auto sections = roomeq::voicingSections (v);
            for (int i = 0; i < sections.count && n < EqChain::maxSections; ++i)
                c[static_cast<std::size_t> (n++)] = designSvf (sections.bands[static_cast<std::size_t> (i)], fs);
        }
    voicingChain.setTargets (c.data(), n);
}

void EqStages::process (float* const* channels, int numChannels, int numSamples, const EqSettings& settings) noexcept
{
    syncSettings (settings);
    correctionChain.process (channels, numChannels, numSamples);
    voicingChain.process (channels, numChannels, numSamples);
}

void EqStages::skip (const EqSettings& settings) noexcept
{
    syncSettings (settings);
    correctionChain.jumpToTargets();
    voicingChain.jumpToTargets();
    correctionChain.reset();
    voicingChain.reset();
}

void EqStages::syncSettings (const EqSettings& settings) noexcept
{
    const auto newCorrection = pending.read (correction);
    if (newCorrection || settings.correctionOn != current.correctionOn || ! same (settings.amount, current.amount))
    {
        current.correctionOn = settings.correctionOn;
        current.amount = settings.amount;
        updateCorrectionTargets();
    }
    if (settings.voicingOn != current.voicingOn || settings.voicing != current.voicing)
    {
        current.voicingOn = settings.voicingOn;
        current.voicing = settings.voicing;
        updateVoicingTargets();
    }
}
