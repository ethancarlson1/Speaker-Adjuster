#include "plugin/LoudnessStage.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace
{
bool same (double a, double b) noexcept { return std::equal_to<double>() (a, b); }

bool sameSettings (const LoudnessSettings& a, const LoudnessSettings& b) noexcept
{
    const auto& x = a.config;
    const auto& y = b.config;
    return a.on == b.on && a.useMic == b.useMic && same (a.hpBaseHz, b.hpBaseHz) && same (x.referenceSpl, y.referenceSpl)
           && same (x.amount, y.amount) && same (x.maxLowDb, y.maxLowDb) && same (x.maxHighDb, y.maxHighDb)
           && same (x.speedS, y.speedS) && x.hpTrack == y.hpTrack;
}
} // namespace

void LoudnessStage::prepare (double sampleRate, int maxBlockSize, const LoudnessSettings& settings)
{
    fs = sampleRate;
    abortTapWhileStopped();
    pendingModel.read (model);
    outputTracker.prepare (fs, settings.config);
    micTracker.prepare (fs, settings.config);
    deadband.reset();
    chain.prepare (fs, 0.25);   // slower than the EQ stages: the level moves in 400 ms steps
    mono.assign (static_cast<std::size_t> (std::max (maxBlockSize, 1)), 0.0f);
    last = settings;
    lowGain = highGain = 0.0;
    haveTargets = false;
    updateTargets (settings, false);
    chain.jumpToTargets();
    status.hasLevel = false;
}

void LoudnessStage::updateTargets (const LoudnessSettings& settings, bool suspend) noexcept
{
    std::array<SvfCoeffs, EqChain::maxSections> c {};
    int n = 0;
    const auto& cfg = settings.config;
    if (settings.on && ! suspend)
    {
        if (model.hasPlan && model.calibrated && deadband.hasLevel())
        {
            const auto [low, high] = roomeq::shelfGains (model.plan, deadband.level(), cfg);
            lowGain = low;
            highGain = high;
        }
        else
        {
            lowGain = highGain = 0.0;
        }
        if (lowGain > 0.0 || highGain > 0.0)
        {
            c[static_cast<std::size_t> (n++)] = designSvf ({ roomeq::BandKind::lowShelf, model.plan.lowFreq, lowGain, model.plan.lowQ }, fs);
            c[static_cast<std::size_t> (n++)] = designSvf ({ roomeq::BandKind::highShelf, model.plan.highFreq, highGain, model.plan.highQ }, fs);
        }
        if (cfg.hpTrack)
        {
            const auto hp = roomeq::trackingHighpass (settings.hpBaseHz, lowGain, cfg);
            c[static_cast<std::size_t> (n++)] = designSvf (hp[0], fs);
            c[static_cast<std::size_t> (n++)] = designSvf (hp[1], fs);
            status.hpFreq = static_cast<float> (hp[0].freq);
        }
        else
        {
            status.hpFreq = 0.0f;
        }
    }
    else
    {
        lowGain = highGain = 0.0;
        status.hpFreq = 0.0f;
    }
    chain.setTargets (c.data(), n);
    status.lowGainDb = static_cast<float> (lowGain);
    status.highGainDb = static_cast<float> (highGain);
    status.lowFreq = static_cast<float> (model.plan.lowFreq);
    status.lowQ = static_cast<float> (model.plan.lowQ);
    status.highFreq = static_cast<float> (model.plan.highFreq);
    status.highQ = static_cast<float> (model.plan.highQ);
    haveTargets = true;
}

void LoudnessStage::process (float* const* channels, int numChannels, const float* mic, int numSamples,
                             const LoudnessSettings& settings, bool suspend, bool snap) noexcept
{
    auto changed = pendingModel.read (model);
    if (! sameSettings (settings, last) || suspend != lastSuspend)
    {
        outputTracker.setConfig (settings.config);
        micTracker.setConfig (settings.config);
        changed = true;
    }
    if (settings.useMic != last.useMic)
        deadband.reset();   // the other source's level is a different number
    last = settings;
    lastSuspend = suspend;

    for (int offset = 0; offset < numSamples;)
    {
        const auto n = std::min (numSamples - offset, static_cast<int> (mono.size()));
        if (! suspend && numChannels > 0)
        {
            // The level of what the speakers are about to get, before this stage's EQ.
            const auto scale = 1.0f / static_cast<float> (std::min (numChannels, 2));
            for (int i = 0; i < n; ++i)
            {
                auto s = channels[0][offset + i];
                if (numChannels > 1)
                    s += channels[1][offset + i];
                mono[static_cast<std::size_t> (i)] = s * scale;
            }
            const auto windows = outputTracker.process (mono.data(), n);
            if (mic != nullptr)
                micTracker.process (mic + offset, n, &outputTracker);
            if (windows > 0 && model.calibrated)
            {
                const auto useMic = settings.useMic && model.calibration.hasMic && micTracker.hasEstimate();
                if (useMic || outputTracker.hasEstimate())
                {
                    const auto spl = useMic ? model.calibration.splFromMic (micTracker.estimate())
                                            : model.calibration.splFromOutput (outputTracker.estimate());
                    deadband.update (spl, settings.config);
                    status.splNow = static_cast<float> (spl);
                    status.splUsed = static_cast<float> (deadband.level());
                    status.hasLevel = true;
                    changed = true;
                }
            }
        }
        offset += n;
    }

    if (changed || ! haveTargets)
        updateTargets (settings, suspend);
    if (suspend && snap && chain.isGliding())
    {
        chain.jumpToTargets();
        chain.reset();
    }
    chain.process (channels, numChannels, numSamples);
}

std::vector<roomeq::Band> loudnessBands (const LoudnessStage::Status& s)
{
    std::vector<roomeq::Band> out;
    const auto low = static_cast<double> (s.lowGainDb.load());
    const auto high = static_cast<double> (s.highGainDb.load());
    if (low > 0.0 || high > 0.0)
    {
        out.push_back ({ roomeq::BandKind::lowShelf, s.lowFreq.load(), low, s.lowQ.load() });
        out.push_back ({ roomeq::BandKind::highShelf, s.highFreq.load(), high, s.highQ.load() });
    }
    if (const auto hp = static_cast<double> (s.hpFreq.load()); hp > 0.0)
        for (const auto& b : roomeq::trackingHighpass (hp, 0.0, {}))   // no rise: hp is already where it sits
            out.push_back (b);
    return out;
}
