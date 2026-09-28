#pragma once

#include "plugin/CaptureList.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ResponseGraph.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

// Phase 1 UI: measurement controls on the left; capture list with grades and
// the response graph (each position, the average, the target) on the right.
class AdaptiveRoomEQEditor final : public juce::AudioProcessorEditor,
                                   private juce::Timer,
                                   private juce::ChangeListener
{
public:
    explicit AdaptiveRoomEQEditor (AdaptiveRoomEQProcessor&);
    ~AdaptiveRoomEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshFromEngine();
    void showResult (const juce::Result& result);
    void redo (int id);
    void drawMeter (juce::Graphics&, juce::Rectangle<int> area, const juce::String& label, float levelDb) const;

    // Standalone only: pick the physical mic input and speaker output as single
    // channels (JUCE's own audio settings dialog only offers stereo pairs).
    void refreshDeviceChannels();
    void applyDeviceChannels();
    juce::String summaryLine() const;

    juce::Rectangle<int> headerArea() const;
    juce::Rectangle<int> controlsArea() const;
    juce::Rectangle<int> statusArea() const;
    juce::Rectangle<int> summaryArea() const;

    AdaptiveRoomEQProcessor& processor;

    juce::ComboBox sweepLength, sweepsPerPosition, sweepSpeaker, smoothing;
    juce::Slider sweepLevel { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label sweepLengthLabel, sweepsLabel, speakerLabel, levelLabel, smoothingLabel;
    std::unique_ptr<ComboAttachment> sweepLengthAttachment, sweepsAttachment, speakerAttachment, smoothingAttachment;
    std::unique_ptr<SliderAttachment> levelAttachment;

    juce::AudioDeviceManager* deviceManager = nullptr;
    juce::ComboBox micInput, speakerOutput;
    juce::Label micInputLabel, speakerOutputLabel;

    juce::TextButton measureButton { "Measure position" };
    juce::TextButton programButton { "Measure from music (30 s)" };
    juce::TextButton stopButton { "Stop" };

    CaptureList captureList;
    ResponseGraph graph;
    juce::TooltipWindow tooltips { this, 600 };

    juce::String errorText;
    float micLevelDb = -100.0f;
    float outputLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQEditor)
};
