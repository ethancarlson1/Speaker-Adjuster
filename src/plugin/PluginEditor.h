#pragma once

#include "plugin/AlignmentPanel.h"
#include "plugin/CaptureList.h"
#include "plugin/PluginProcessor.h"
#include "plugin/QualityCard.h"
#include "plugin/ResponseGraph.h"
#include "plugin/SystemSummaryPanel.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// Three modes (the header's Setup / Tune / Show), each with its pages over a
// status area that is always visible:
// - Setup: Zone (and, in the app, the mic input and speaker output) and
//   Level Compensation (plugin only).
// - Tune: 1 Measure, 2 Align (plugin only), 3 Correct, 4 Verify. On the
//   right, the capture list (the system summary on Correct) and the graph
//   (response on top, EQ stages below).
// - Show (plugin only): Monitor (the bypasses, the level readout and the SPL
//   meter) and Engineer Voicing; the graph shows the mic's spectrogram with
//   the EQ curves over it.
// Advanced shows the detailed limits and the raw numbers; without it the
// pages keep to what a tuning needs.
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
        zone,
        align,
        verify,
        monitor
    };
    enum class Mode
    {
        setup = 0,
        tune,
        show
    };
    void showTab (Tab tab);                  // switches to its mode too
    void setMode (Mode m);                   // to the page last open in it
    Mode getMode() const { return mode; }
    void setAdvanced (bool on);
    bool isAdvanced() const { return advanced; }
    void selectVoicingBand (int band);
    void setShowView (bool on);              // Show mode, or back to Tune
    bool isShowView() const { return showViewOn; }

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
    void chooseTargetFile (bool importing, const juce::String& extension = {});
    std::unique_ptr<juce::FileChooser> targetChooser;

    // Standalone only: pick the physical mic input and speaker output as single
    // channels (JUCE's own audio settings dialog only offers stereo pairs).
    void refreshDeviceChannels();
    void applyDeviceChannels();
    void updateSignalRows();   // sweep length/count vs noise length
    void updateTabVisibility();
    juce::String summaryLine() const;
    juce::String correctionInfo() const;
    juce::String verifyInfo() const;
    juce::String verifiedText() const;   // the verified average's distance from the target (empty if none)
    static Mode modeOf (Tab tab);
    std::vector<juce::TextButton*> tabsOf (Mode m);
    juce::String loudnessInfo() const;
    juce::String zoneInfo() const;
    juce::String statusText() const;
    void submitSpl();
    void updateLoudnessControls();
    juce::String showLoudnessText() const;
    void drawSpl (juce::Graphics&, juce::Rectangle<int> area) const;

    juce::Rectangle<int> headerArea() const;
    juce::Rectangle<int> controlsArea() const;
    juce::Rectangle<int> summaryArea() const;

    AdaptiveRoomEQProcessor& processor;

    // Modes, and Advanced.
    juce::TextButton setupButton { "Setup" }, tuneButton { "Tune" }, showButton { "Show" };
    juce::ToggleButton advancedToggle { "Advanced" };
    Mode mode = Mode::tune;
    std::array<Tab, 3> lastTab { Tab::zone, Tab::measure, Tab::monitor };   // per mode
    bool advanced = false;
    std::vector<juce::Component*> advancedControls;   // shown only with Advanced on (on their page)

    // Show mode.
    bool showViewOn = false;
    std::vector<juce::Component*> showControls;
    juce::TextButton showRecheckButton { "Re-check level" }, splResetButton { "Reset" };
    juce::ToggleButton showCorrectionOn { "System Correction" }, showVoicingOn { "Engineer Voicing" },
        showLoudOn { "Level Compensation" }, showLevelMatch { "Match output level" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> showCorrectionAttachment, showVoicingAttachment,
        showLoudAttachment, showLevelMatchAttachment;
    juce::Rectangle<int> showLoudBounds, splBounds;
    int splRuleY = 0;

    // Tabs. Five share the panel's width, so their text is a little smaller than a button's.
    struct TabLook : juce::LookAndFeel_V4
    {
        juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::FontOptions (tabFontHeight); }
        static constexpr float tabFontHeight = 14.0f;
    } tabLook;
    juce::TextButton zoneTab { "Zone" }, measureTab { "Measure" }, correctTab { "Correct" }, voicingTab { "Engineer Voicing" },
        loudnessTab { "Level Compensation" }, alignTab { "Align" }, verifyTab { "Verify" }, monitorTab { "Monitor" };
    Tab currentTab = Tab::measure;
    std::vector<juce::Component*> zoneControls, measureControls, correctControls, voicingControls, loudnessControls,
        alignControls, verifyControls;
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
    juce::ToggleButton correctionOn { "System Correction" }, levelMatch { "Match output level" };
    juce::Slider amount { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxCut { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        maxBoost { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeLo { juce::Slider::LinearBar, juce::Slider::TextBoxRight },
        rangeHi { juce::Slider::LinearBar, juce::Slider::TextBoxRight };
    juce::Label amountLabel, maxCutLabel, maxBoostLabel, rangeLoLabel, rangeHiLabel;
    juce::TextButton applyButton { "Apply System Correction" }, compareButton { "Hear previous" }, undoButton { "Undo" },
        verifyButton { "Verify: measure through the EQ" }, exportButton { juce::String::fromUTF8 ("Export\xe2\x80\xa6") };
    std::unique_ptr<juce::FileChooser> exportChooser;
    void showExportMenu();
    void saveExport (const juce::String& extension);
    std::unique_ptr<ComboAttachment> targetAttachment;
    std::unique_ptr<ButtonAttachment> correctionOnAttachment, levelMatchAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment, maxCutAttachment, maxBoostAttachment, rangeLoAttachment,
        rangeHiAttachment;

    // Voicing tab.
    juce::ToggleButton voicingOn { "Engineer Voicing" };
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
    juce::ToggleButton loudOn { "Level Compensation" }, loudHighPass { "Protective high-pass (follows the boost)" };
    juce::ComboBox loudStrength;   // Subtle / Natural / Full: presets for Amount
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
    std::unique_ptr<AlignmentPanel> alignment;   // plugin only
public:
    AlignmentPanel* getAlignmentPanel() { return alignment.get(); }   // tests
private:

    CaptureList captureList;
    QualityCard qualityCard;                     // Measure tab: the selected capture's quality
    void updateQualityCard();
    // Correct tab: the system summary where the capture list is, or the list.
    SystemSummaryPanel systemPanel;
    juce::TextButton summaryButton { "System summary" }, capturesButton { "Captures" };
    bool showingSummary = true;
public:
    QualityCard& getQualityCard() { return qualityCard; }         // tests
    CaptureList& getCaptureList() { return captureList; }         // tests
    SystemSummaryPanel& getSystemPanel() { return systemPanel; }  // tests
    juce::TextButton& getCapturesButton() { return capturesButton; }
    juce::TextButton& getSummaryButton() { return summaryButton; }
    bool isPageButtonShown (Tab t);                               // tests: the page's button is in the tab bar
    const std::vector<juce::Component*>& getAdvancedControls() const { return advancedControls; }
    juce::TextButton& getModeButton (Mode m) { return m == Mode::setup ? setupButton : m == Mode::show ? showButton : tuneButton; }
    void copyEqToClipboard();                                     // Export... > Copy (tests too)
private:
    ResponseGraph graph { processor };
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::AlertWindow> saveDialog;

    juce::String errorText;
    juce::String noticeText;   // a done message (shown like the status for a few seconds)
    juce::uint32 noticeTime = 0;
    float micLevelDb = -100.0f;
    float outputLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQEditor)
};
