#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace
{
namespace ParamIds
{
const juce::ParameterID sweepLength { "sweepLength", 1 };
const juce::ParameterID sweepsPerPosition { "sweepsPerPosition", 1 };
const juce::ParameterID sweepSpeaker { "sweepSpeaker", 1 };
const juce::ParameterID sweepLevel { "sweepLevel", 1 };
const juce::ParameterID smoothing { "smoothing", 1 };
} // namespace ParamIds

constexpr double sweepSeconds[] = { 2.0, 5.0, 10.0 };
constexpr int smoothingFractions[] = { 3, 4, 6 };

bool loadedAsStandalone()
{
    return juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone;
}

// Lock-free "max" so the editor can read-and-reset peaks between blocks.
void updatePeak (std::atomic<float>& peak, float value) noexcept
{
    auto current = peak.load (std::memory_order_relaxed);
    while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
    {
    }
}
} // namespace

juce::AudioProcessor::BusesProperties AdaptiveRoomEQProcessor::makeBuses (bool isStandalone)
{
    // The standalone app is a measurement tool: its only input is the mic, and
    // the user picks the physical mic input and speaker output in the editor.
    if (isStandalone)
        return BusesProperties()
            .withInput ("Measurement Mic", juce::AudioChannelSet::mono(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);

    return BusesProperties()
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Measurement Mic", juce::AudioChannelSet::mono(), true);
}

AdaptiveRoomEQProcessor::AdaptiveRoomEQProcessor()
    : AudioProcessor (makeBuses (loadedAsStandalone())),
      standalone (loadedAsStandalone()),
      parameters (*this, nullptr, "Parameters", createParameterLayout())
{
    // Parameter callbacks can arrive on the audio thread, so the engine polls
    // the smoothing choice from its message-thread timer instead.
    engine.setSmoothingSource ([this] { return getSmoothingFraction(); });
}

juce::AudioProcessorValueTreeState::ParameterLayout AdaptiveRoomEQProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepLength, "Sweep length",
                                                        StringArray { "2 s", "5 s", "10 s" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepsPerPosition, "Sweeps per position",
                                                        StringArray { "1", "2", "3" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::sweepSpeaker, "Sweep speaker",
                                                        StringArray { "Left", "Right" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParamIds::sweepLevel, "Sweep level",
                                                       NormalisableRange<float> (-40.0f, 0.0f, 0.5f), -12.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dBFS")));
    layout.add (std::make_unique<AudioParameterChoice> (ParamIds::smoothing, "Smoothing",
                                                        StringArray { "1/3 octave", "1/4 octave", "1/6 octave" }, 2));
    return layout;
}

void AdaptiveRoomEQProcessor::prepareToPlay (double, int)
{
    // Audio is stopped here, so an unfinished measurement can be dropped safely
    // (its sample rate may no longer match).
    recorder.abortWhileStopped();
}

void AdaptiveRoomEQProcessor::releaseResources()
{
    recorder.abortWhileStopped();
}

bool AdaptiveRoomEQProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::stereo())
        return false;

    if (standalone)
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono();

    if (layouts.getMainInputChannelSet() != mainOut)
        return false;

    // The mic sidechain is mono. Hosts may also leave it disconnected (disabled).
    // Some hosts only offer stereo sidechains; accept that and use the first channel.
    if (layouts.inputBuses.size() > micBusIndex)
    {
        const auto mic = layouts.getChannelSet (true, micBusIndex);
        if (! mic.isDisabled() && mic != juce::AudioChannelSet::mono() && mic != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

const float* AdaptiveRoomEQProcessor::findMic (juce::AudioBuffer<float>& buffer)
{
    if (standalone)
        return getTotalNumInputChannels() > 0 ? buffer.getReadPointer (0) : nullptr;

    if (auto* micBus = getBus (true, micBusIndex); micBus != nullptr && micBus->isEnabled())
    {
        const auto mic = getBusBuffer (buffer, true, micBusIndex);
        if (mic.getNumChannels() > 0)
            return mic.getReadPointer (0);
    }
    return nullptr;
}

void AdaptiveRoomEQProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();

    // Plugin: the mic sidechain sits on a channel above the main outputs.
    // Standalone: the mic is input channel 0, which shares memory with output
    // channel 0; the recorder reads each mic sample before writing that output
    // sample, so the sweep never overwrites the recording.
    auto mainOut = getBusBuffer (buffer, false, 0);
    const auto* mic = findMic (buffer);
    if (mic != nullptr)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (mic, numSamples);
        updatePeak (micPeak, juce::jmax (range.getEnd(), -range.getStart()));
    }

    const auto wroteOutput = recorder.process (mainOut.getArrayOfWritePointers(), mainOut.getNumChannels(), mic, numSamples);

    // The standalone app never passes its input (the mic) to the speakers.
    if (standalone && ! wroteOutput)
        mainOut.clear();

    updatePeak (outputPeak, mainOut.getMagnitude (0, numSamples));
}

bool AdaptiveRoomEQProcessor::isMicConnected() const
{
    if (standalone)
        return getTotalNumInputChannels() > 0;
    const auto* micBus = getBus (true, micBusIndex);
    return micBus != nullptr && micBus->isEnabled();
}

MeasurementEngine::SweepSettings AdaptiveRoomEQProcessor::getSweepSettings() const
{
    MeasurementEngine::SweepSettings s;
    const auto index = [this] (const juce::ParameterID& id)
    { return juce::roundToInt (parameters.getRawParameterValue (id.getParamID())->load()); };
    s.seconds = sweepSeconds[juce::jlimit (0, 2, index (ParamIds::sweepLength))];
    s.repeats = juce::jlimit (0, 2, index (ParamIds::sweepsPerPosition)) + 1;
    // Standalone: the speaker is chosen as a physical output in the editor, and
    // the sweep always plays on the first output channel.
    s.channel = standalone ? 0 : juce::jlimit (0, 1, index (ParamIds::sweepSpeaker));
    s.levelDbfs = parameters.getRawParameterValue (ParamIds::sweepLevel.getParamID())->load();
    return s;
}

int AdaptiveRoomEQProcessor::getSmoothingFraction() const
{
    const auto index = juce::roundToInt (parameters.getRawParameterValue (ParamIds::smoothing.getParamID())->load());
    return smoothingFractions[juce::jlimit (0, 2, index)];
}

juce::Result AdaptiveRoomEQProcessor::startSweep (int replaceId)
{
    if (! isMicConnected())
        return juce::Result::fail ("Connect the measurement mic first");
    return engine.startSweep (getSampleRate(), getSweepSettings(), replaceId);
}

juce::Result AdaptiveRoomEQProcessor::startProgram (int replaceId)
{
    if (standalone)
        return juce::Result::fail ("Measuring from music needs the program to pass through the plugin, "
                                   "so it's only available in the plugin inside your DAW.");
    if (! isMicConnected())
        return juce::Result::fail ("Connect the measurement mic first");
    return engine.startProgram (getSampleRate(), programSeconds, replaceId);
}

juce::AudioProcessorEditor* AdaptiveRoomEQProcessor::createEditor()
{
    return new AdaptiveRoomEQEditor (*this);
}

void AdaptiveRoomEQProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("AdaptiveRoomEQ");
    root.setProperty ("stateVersion", 1, nullptr);
    root.appendChild (parameters.copyState(), nullptr);
    root.appendChild (engine.toValueTree(), nullptr);
    juce::MemoryOutputStream stream (destData, false);
    root.writeToStream (stream);
}

void AdaptiveRoomEQProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto root = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
    if (! root.hasType ("AdaptiveRoomEQ"))
        return;
    if (const auto params = root.getChildWithName (parameters.state.getType()); params.isValid())
        parameters.replaceState (params);
    engine.fromValueTree (root.getChildWithName (MeasurementEngine::treeType));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaptiveRoomEQProcessor();
}
