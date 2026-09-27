#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace
{
// Lock-free "max" so the editor can read-and-reset peaks between blocks.
void updatePeak (std::atomic<float>& peak, float value) noexcept
{
    auto current = peak.load (std::memory_order_relaxed);
    while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
    {
    }
}
} // namespace

AdaptiveRoomEQProcessor::AdaptiveRoomEQProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Measurement Mic", juce::AudioChannelSet::mono(), true))
{
}

void AdaptiveRoomEQProcessor::prepareToPlay (double, int)
{
}

void AdaptiveRoomEQProcessor::releaseResources()
{
}

bool AdaptiveRoomEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::stereo() || layouts.getMainInputChannelSet() != mainOut)
        return false;

    // The mic sidechain is mono. Hosts may also leave it disconnected (disabled).
    // Some hosts only offer stereo sidechains; accept that and use the first channel.
    if (layouts.inputBuses.size() > micBusIndex)
    {
        const auto mic = layouts.getChannelSet (true, micBusIndex);

        if (! mic.isDisabled()
            && mic != juce::AudioChannelSet::mono()
            && mic != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void AdaptiveRoomEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Main bus passes through untouched until the EQ stages exist.
    // The main output shares channels 0-1 with the main input, so there is nothing to copy.
    const auto mainOut = getBusBuffer (buffer, false, 0);
    updatePeak (outputPeak, mainOut.getMagnitude (0, mainOut.getNumSamples()));

    if (auto* micBus = getBus (true, micBusIndex); micBus != nullptr && micBus->isEnabled())
    {
        const auto mic = getBusBuffer (buffer, true, micBusIndex);

        if (mic.getNumChannels() > 0)
            updatePeak (micPeak, mic.getMagnitude (0, 0, mic.getNumSamples()));
    }
}

bool AdaptiveRoomEQProcessor::isMicBusEnabled() const
{
    const auto* micBus = getBus (true, micBusIndex);
    return micBus != nullptr && micBus->isEnabled();
}

juce::AudioProcessorEditor* AdaptiveRoomEQProcessor::createEditor()
{
    return new AdaptiveRoomEQEditor (*this);
}

void AdaptiveRoomEQProcessor::getStateInformation (juce::MemoryBlock&)
{
    // Measurements, calibration and curves are saved here once the analysis engine is ported.
}

void AdaptiveRoomEQProcessor::setStateInformation (const void*, int)
{
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaptiveRoomEQProcessor();
}
