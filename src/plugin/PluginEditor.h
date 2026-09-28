#pragma once

#include "plugin/CaptureList.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ResponseGraph.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// Left: three tabs (Measure, Correct, Voicing) over a status area that is
// always visible. Right: the capture list with grades, and the graph
// (response on top, the two EQ stages below).
class AdaptiveRoomEQEditor final : public juce::AudioProcessorEditor,
                                   private juce::Timer,
                                   private juce::ChangeListener
{
public:
    explicit AdaptiveRoomEQEditor (AdaptiveRoomEQProcessor&);
    ~AdaptiveRoomEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    enum class Tab
    {
        measure = 0,
        correct,
        voicing
    };
    void showTab (Tab tab);
    void selectVoicingBand (int band);

private:
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshFromEngine();
    void showResult (const juce::Result& result);
    void redo (int id);
    void drawMeter (juce::Graphics&, juce::Rectangle<int> area, const juce::String& label, float levelDb) const;
    void showTargetMenu();
    void askToSaveTarget();

    // Standalone only: pick the physical mic input and speaker output as single
    // channels (JUCE's own audio settings dialog only offers stereo pairs).
    void refreshDeviceChannels();
    void applyDeviceChannels();
    void updateSignalRows();   // sweep length/count vs noise length
    void updateTabVisibility();
    juce::String summaryLine() const;
    juce::String correctionInfo() const;

    juce::Rectangle<int> headerArea() const;
    juce::Rectangle<int> controlsArea() const;
    juce::Rectangle<int> summaryArea() const;

    AdaptiveRoomEQProcessor& processor;

    // Tabs.
    juce::TextButton measureTab { "Measure" }, correctTab { "Correct" }, voicingTab { "Voicing" };
    Tab currentTab = Tab::measure;
    std::vector<juce::Component*> measureControls, correctControls, voicingControls;
    juce::Rectangle<int> statusBounds, tipBounds, infoBounds;

    // Measure tab.
    juce::ComboBox signal, sweepLength, sweepsPerPosition, noiseLength, sweepSpeaker, smoothing;
    juce::Slider sweepLevel { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label signalLabel, sweepLengthLabel, sweepsLabel, noiseLengthLabel, speakerLabel, levelLabel, smoothingLabel;
    std::unique_ptr<ComboAttachment> signalAttachment, sweepLengthAttachment, sweepsAttachment, noiseLengthAttachment,
        speakerAttachment, smoothingAttachment;
    bool showingNoiseRows = false;
    std::unique_ptr<SliderAttachment> levelAttachment;

    juce::AudioDeviceManager* deviceManager = nullptr;
    juce::ComboBox micInput, speakerOutput;
    juce::Label micInputLabel, speakerOutputLabel;

    juce::TextButton measureButton { "Measure position" };
    juce::TextButton programButton { "Measure from music (30 s)" };
    juce::TextButton stopButton { "Stop" };

    // Correct tab.
    juce::ComboBox target;
    juce::Label targetLabel;
    juce::TextButton targetMenu { juce::String::fromUTF8 ("Targets\xe2\x80\xa6") };
    juce::ToggleButton correctionOn { "Correction on" };
    juce::Slider amount { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxCut { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxBoost { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeLo { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeHi { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label amountLabel, maxCutLabel, maxBoostLabel, rangeLoLabel, rangeHiLabel;
    juce::TextButton applyButton { "Apply correction" }, compareButton { "Hear previous" }, undoButton { "Undo" },
        verifyButton { "Verify: measure through the EQ" };
    std::unique_ptr<ComboAttachment> targetAttachment;
    std::unique_ptr<ButtonAttachment> correctionOnAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment, maxCutAttachment, maxBoostAttachment, rangeLoAttachment,
        rangeHiAttachment;

    // Voicing tab.
    juce::ToggleButton voicingOn { "Voicing EQ on" };
    std::unique_ptr<ButtonAttachment> voicingOnAttachment;
    std::array<juce::TextButton, roomeq::numVoicingBands> bandButtons;
    juce::ToggleButton bandOn { "Band on" };
    juce::ComboBox bandType;
    juce::Slider bandFreq { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        bandGain { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        bandQ { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label bandTypeLabel, bandFreqLabel, bandGainLabel, bandQLabel;
    std::unique_ptr<ButtonAttachment> bandOnAttachment;
    std::unique_ptr<ComboAttachment> bandTypeAttachment;
    std::unique_ptr<SliderAttachment> bandFreqAttachment, bandGainAttachment, bandQAttachment;
    int selectedBand = 0;

    CaptureList captureList;
    ResponseGraph graph { processor };
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::AlertWindow> saveDialog;

    juce::String errorText;
    float micLevelDb = -100.0f;
    float outputLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQEditor)
};
