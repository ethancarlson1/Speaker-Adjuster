#pragma once

#include "plugin/CaptureList.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ResponseGraph.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// The show-tracking warning: a strip across the top of the graph side, yellow
// for a change of 3 dB or more since soundcheck, red for 6 dB or more. Click
// it for the band-by-band details. Hidden when nothing's flagged.
class ShowBanner final : public juce::Component,
                         public juce::SettableTooltipClient
{
public:
    std::function<void()> onClick;
    std::function<void()> onDismiss;   // the × at the right
    void set (int newSeverity, const juce::String& newText);
    int getSeverity() const { return severity; }
    const juce::String& getText() const { return text; }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;
    juce::Rectangle<float> dismissArea() const;

private:
    int severity = 0;
    juce::String text;
};

// Setup view: tabs (Zone, Measure, Correct, Voicing, Loudness; the standalone
// app has no Loudness tab, and its Zone tab has no delay or polarity) over a
// status area that is always visible; on the right the
// capture list with grades, and the graph (response on top, EQ stages below).
// Show view (the header's button, plugin only): the left panel holds show
// tracking and the bypasses, the capture list goes, and the graph's top panel
// shows the change since soundcheck. The warning banner shows in both.
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
        voicing,
        loudness,
        zone
    };
    void showTab (Tab tab);
    void selectVoicingBand (int band);
    void setShowView (bool on);
    bool isShowView() const { return showViewOn; }
    const ShowBanner& getBanner() const { return banner; }

private:
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refreshFromEngine();
    void showResult (const juce::Result& result);
    bool micReady();                 // false (with a warning popup) if a measurement couldn't hear anything
    void redo (int id);
    void drawMeter (juce::Graphics&, juce::Rectangle<int> area, const juce::String& label, float levelDb,
                    bool withTarget = false) const;
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
    juce::String loudnessInfo() const;
    juce::String zoneInfo() const;
    juce::String statusText() const;
    void submitSpl();
    void updateLoudnessControls();
    void updateBanner();
    void showBannerDetails();
    juce::String showTrackingText() const;
    juce::String showLoudnessText() const;
    void drawSpl (juce::Graphics&, juce::Rectangle<int> area) const;

    juce::Rectangle<int> headerArea() const;
    juce::Rectangle<int> controlsArea() const;
    juce::Rectangle<int> summaryArea() const;

    AdaptiveRoomEQProcessor& processor;

    // Show view and the warning banner.
    juce::TextButton showViewButton { "Show view" };
    ShowBanner banner;
    bool showViewOn = false;
    std::vector<juce::Component*> showControls;
    juce::TextButton storeRefButton { "Store reference" }, clearRefButton { "Clear" }, showRecheckButton { "Re-check level" },
        splResetButton { "Reset" };
    juce::ToggleButton showCorrectionOn { "Correction on" }, showVoicingOn { "Voicing EQ on" }, showLoudOn { "Loudness on" },
        showLevelMatch { "Match output level" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> showCorrectionAttachment, showVoicingAttachment,
        showLoudAttachment, showLevelMatchAttachment;
    juce::Rectangle<int> showTextBounds, showLoudBounds, splBounds;
    int splRuleY = 0;
    int showRuleY = 0;

    // Tabs. Five share the panel's width, so their text is a little smaller than a button's.
    struct TabLook : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::FontOptions (tabFontHeight); }
        static constexpr float tabFontHeight = 14.0f;
    } tabLook;
    juce::TextButton zoneTab { "Zone" }, measureTab { "Measure" }, correctTab { "Correct" }, voicingTab { "Voicing" },
        loudnessTab { "Loudness" };
    Tab currentTab = Tab::measure;
    std::vector<juce::Component*> zoneControls, measureControls, correctControls, voicingControls, loudnessControls;
    juce::Rectangle<int> statusBounds, tipBounds, infoBounds;
    int calibrationRuleY = 0;

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
    juce::TextButton clearAllButton { juce::String::fromUTF8 ("Clear all\xe2\x80\xa6") };
    juce::TextButton stopButton { "Stop" };

    // Correct tab.
    juce::ComboBox target;
    juce::Label targetLabel;
    juce::TextButton targetMenu { juce::String::fromUTF8 ("Targets\xe2\x80\xa6") };
    juce::ToggleButton correctionOn { "Correction on" }, levelMatch { "Match output level" };
    juce::Slider amount { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxCut { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxBoost { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeLo { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeHi { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label amountLabel, maxCutLabel, maxBoostLabel, rangeLoLabel, rangeHiLabel;
    juce::TextButton applyButton { "Apply correction" }, compareButton { "Hear previous" }, undoButton { "Undo" },
        verifyButton { "Verify: measure through the EQ" };
    std::unique_ptr<ComboAttachment> targetAttachment;
    std::unique_ptr<ButtonAttachment> correctionOnAttachment, levelMatchAttachment;
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

    // Loudness tab.
    juce::ToggleButton loudOn { "Loudness compensation on" }, loudHighPass { "Protective high-pass (follows the boost)" };
    juce::Slider loudRef { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        loudAmount { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        loudMaxLow { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        loudMaxHigh { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        loudSpeed { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::ComboBox loudSource, calibratorLevel;
    juce::Label loudRefLabel, loudAmountLabel, loudMaxLabel, loudSourceLabel, loudSpeedLabel, splLabel, calibratorLabel;
    juce::TextEditor splEntry;
    juce::TextButton calibrateButton { "Calibrate level: play noise on both speakers" }, setSplButton { "Set" },
        micCalButton { "Calibrate mic" }, recheckButton { "Re-check level from the music" };
    std::unique_ptr<ButtonAttachment> loudOnAttachment, loudHighPassAttachment;
    std::unique_ptr<ComboAttachment> loudSourceAttachment;
    std::unique_ptr<SliderAttachment> loudRefAttachment, loudAmountAttachment, loudMaxLowAttachment, loudMaxHighAttachment,
        loudSpeedAttachment;
    LoudnessController::Step lastLoudnessStep = LoudnessController::Step::idle;

    // Zone tab.
    juce::ComboBox zone;
    juce::Label zoneLabel, zoneDelayLabel;
    juce::Slider zoneDelay { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::ToggleButton polarity { "Invert polarity" };
    std::unique_ptr<ComboAttachment> zoneAttachment;
    std::unique_ptr<SliderAttachment> zoneDelayAttachment;
    std::unique_ptr<ButtonAttachment> polarityAttachment;
    bool lastMono = false;

    CaptureList captureList;
    ResponseGraph graph { processor };
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::AlertWindow> saveDialog;

    juce::String errorText;
    float micLevelDb = -100.0f;
    float outputLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQEditor)
};
