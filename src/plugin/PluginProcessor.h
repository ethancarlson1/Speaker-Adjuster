#pragma once

#include "plugin/CaptureRecorder.h"
#include "plugin/MeasurementEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

// Audio path (later phases): measured correction -> voicing EQ -> loudness comp.
//
// Buses: stereo main in/out plus a mono sidechain input for the measurement
// mic. The standalone app can't use sidechains (JUCE disables non-main buses
// there), so it takes a 3-channel main input instead: 1-2 program, 3 mic.
//
// Phase 1: the main path passes through, except while a sweep plays (on one
// speaker; one correction will later be applied to both sides). The
// standalone app outputs only the sweep, never its inputs. Analysis never
// runs on the audio thread.
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
    bool isMicConnected() const;
    bool isStandalone() const { return standalone; }
    float takeMicPeak() noexcept    { return micPeak.exchange (0.0f); }
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f); }

    MeasurementEngine::SweepSettings getSweepSettings() const;
    int getSmoothingFraction() const;
    juce::Result startSweep (int replaceId = -1);
    juce::Result startProgram (int replaceId = -1);

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    MeasurementEngine& getEngine() { return engine; }

    static constexpr int micBusIndex = 1;
    static constexpr double programSeconds = 30.0;

private:
    static BusesProperties makeBuses (bool isStandalone);
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    const float* findMic (juce::AudioBuffer<float>& buffer);

    const bool standalone;
    juce::AudioProcessorValueTreeState parameters;
    CaptureRecorder recorder;
    MeasurementEngine engine { recorder };

    std::atomic<float> micPeak { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQProcessor)
};
