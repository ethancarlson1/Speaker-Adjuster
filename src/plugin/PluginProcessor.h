#pragma once

#include "plugin/CaptureRecorder.h"
#include "plugin/EqStages.h"
#include "plugin/LoudnessController.h"
#include "plugin/LoudnessStage.h"
#include "plugin/MeasurementEngine.h"
#include "plugin/ShowController.h"
#include "plugin/TapRecorder.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <mutex>

// Audio path: measured correction -> voicing EQ -> output level match ->
// loudness compensation, the same on both speakers.
//
// Buses: stereo main in/out plus a mono sidechain input for the measurement
// mic. The standalone app is a measurement tool: its only input is the mic
// (JUCE disables sidechains there), and it outputs only the test signal, on
// the physical output picked in the editor.
//
// Measurements play the test signal on one speaker, straight to the output
// (the fit needs the PA's own response); verify measurements play it through
// the EQ. While any measurement or calibration runs, loudness compensation is
// flat and doesn't track. Analysis and fitting never run on the audio thread.
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
    bool hasMicSignal() const;                                    // the mic carried signal within the last second of audio
    juce::Result checkMicSignal() const;                          // fails, saying what to fix, if a measurement can't hear anything
    bool isStandalone() const { return standalone; }
    float takeMicPeak() noexcept    { return micPeak.exchange (0.0f); }
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f); }

    MeasurementEngine::SweepSettings getSweepSettings() const;
    int getSmoothingFraction() const;
    bool isNoiseSelected() const;
    double getNoiseSeconds() const;

    // Uses the selected signal (sweep or pink noise). Verify plays it through the EQ.
    juce::Result startMeasurement (int replaceId = -1);
    juce::Result startVerify();
    juce::Result startSweep (int replaceId = -1, bool verify = false);
    juce::Result startNoise (int replaceId = -1, bool verify = false);
    juce::Result startProgram (int replaceId = -1);

    // Targets: the three presets, or the session's custom points.
    enum TargetChoice { targetFlat = 0, targetHouse, targetSpeech, targetCustom };
    int getTargetChoice() const;
    roomeq::TargetCurve getTarget() const;
    roomeq::TargetCurve getCustomTarget() const;
    void setCustomTarget (const roomeq::TargetCurve& target);   // points are sanitised
    static juce::File getTargetsFolder();
    juce::Array<juce::File> getSavedTargets() const;
    juce::Result saveCustomTarget (const juce::String& name);
    juce::Result loadTarget (const juce::File& file);             // into Custom, and selects it

    MeasurementEngine::CorrectionSettings getCorrectionSettings() const;
    EqSettings getEqSettings() const noexcept;                    // what the audio path uses now
    std::vector<roomeq::Band> getVoicingSections() const;         // for the graph
    float getMakeupDb() const noexcept { return eq.getMakeupDb(); }   // level match gain now (0 when off)

    LoudnessSettings getLoudnessSettings() const noexcept;        // what the loudness stage uses now
    LoudnessController& getLoudness() { return loudnessControl; }
    ShowController& getShow() { return showControl; }

    // The editor's compact show layout; saved with the session (not a parameter).
    bool isShowView() const noexcept { return showView.load(); }
    void setShowView (bool shouldShow) noexcept { showView = shouldShow; }
    const LoudnessStage::Status& getLoudnessStatus() const { return loudness.getStatus(); }

    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    MeasurementEngine& getEngine() { return engine; }

    static constexpr int micBusIndex = 1;
    static constexpr double programSeconds = 30.0;

private:
    static BusesProperties makeBuses (bool isStandalone);
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    const float* findMic (juce::AudioBuffer<float>& buffer);
    float raw (const juce::ParameterID& id) const noexcept;

    const bool standalone;
    juce::AudioProcessorValueTreeState parameters;
    CaptureRecorder recorder;
    MeasurementEngine engine { recorder };
    EqStages eq;
    LoudnessStage loudness;
    LoudnessController loudnessControl { loudness, engine };
    TapRecorder showTap;                          // the final output and the mic, for show tracking
    ShowController showControl { showTap };
    std::atomic<bool> showView { false };

    // Parameter values the audio thread reads each block.
    struct VoicingParams
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* type = nullptr;
        std::atomic<float>* freq = nullptr;
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* q = nullptr;
    };
    std::atomic<float>* correctionOnParam = nullptr;
    std::atomic<float>* amountParam = nullptr;
    std::atomic<float>* voicingOnParam = nullptr;
    std::atomic<float>* levelMatchParam = nullptr;
    std::array<VoicingParams, roomeq::numVoicingBands> voicingParams;
    struct LoudnessParams
    {
        std::atomic<float>* on = nullptr;
        std::atomic<float>* reference = nullptr;
        std::atomic<float>* amount = nullptr;
        std::atomic<float>* maxLow = nullptr;
        std::atomic<float>* maxHigh = nullptr;
        std::atomic<float>* speed = nullptr;
        std::atomic<float>* source = nullptr;
        std::atomic<float>* highPass = nullptr;
    } loudnessParams;

    mutable std::mutex customLock;
    roomeq::TargetCurve customTarget { "Custom", roomeq::houseTarget().points };

    std::atomic<float> micPeak { 0.0f };
    static constexpr float micSignalFloor = 1e-5f;                // -100 dBFS: below this the mic input is dead
    std::atomic<std::int64_t> samplesSinceMicSignal { std::int64_t { 1 } << 40 };
    std::atomic<float> outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQProcessor)
};
