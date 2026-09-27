#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

// Audio path (later phases): measured correction -> voicing EQ -> loudness comp.
// Bus layout: stereo main in/out, plus a mono sidechain input for the
// measurement mic. Heavy analysis never runs on the audio thread.
class AdaptiveRoomEQProcessor final : public juce::AudioProcessor
{
public:
    AdaptiveRoomEQProcessor();
    ~AdaptiveRoomEQProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Message-thread helpers for the editor.
    bool isMicBusEnabled() const;
    float takeMicPeak() noexcept    { return micPeak.exchange (0.0f); }
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f); }

    static constexpr int micBusIndex = 1;

private:
    std::atomic<float> micPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQProcessor)
};
