#include "plugin/PluginEditor.h"

#include "plugin/Theme.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

namespace
{
constexpr float meterFloorDb = -80.0f;
constexpr float meterDecayDbPerTick = 1.5f;
constexpr int refreshHz = 30;
constexpr int margin = 16;
constexpr int controlsWidth = 330;
constexpr int headerHeight = 60;
constexpr int modeButtonsX = 252;   // after the title
constexpr int statusHeight = 92;
constexpr float micTargetLowDb = -30.0f, micTargetHighDb = -10.0f;   // the Mic meter's green zone, for the mic's peaks

const char* const placementTip =
    "Measure 3-5 spots across the audience area, at different distances and off-axis. "
    "Avoid symmetric spots on the centre line. The signal plays on one speaker; the "
    "correction is applied to both sides.";

const char* const correctTip =
    "Measurements play straight to the speaker, so the fit always sees the PA's own response. "
    "Apply puts the proposal on the audio path; then Verify measures through it.";

const char* const verifyTip =
    "Measure two or three of the same spots again, through the System Correction, the Engineer Voicing and the "
    "zone's delay: the violet curve on the graph. Verify captures never change the proposal.";

const char* const alignTip =
    "Line this zone up with the mains: measure both from where they overlap and apply the suggestion, or start "
    "the Delay at the extra distance the mains' sound travels and fine-tune.";

const char* const voicingTip =
    "Your taste layer after the correction; measuring never changes it. On the graph, drag a "
    "numbered handle: sideways for frequency, up and down for gain. The mouse wheel sets Q; "
    "double-click switches a band on or off.";

const char* const zoneTip =
    "One instance per zone, each on its own track or output. Choosing a zone sets its starting correction range "
    "(Tune > Correct); change it after as you like. Its delay and polarity are in Tune > Align.";

const char* const loudnessTip =
    "When the show plays quieter than the reference level, bass and treble come up by what equal-loudness "
    "behaviour (ISO 226:2003) predicts for that change, so the balance you approved stays similar. Turn the "
    "volume down before the plugin. Calibrate once per setup; re-check if the amp gain changes.";

constexpr float strengthPresets[] = { 50.0f, 75.0f, 100.0f };   // Subtle, Natural, Full (Amount %)

juce::String signedDb (double v)
{
    const auto r = std::round (v * 10.0) / 10.0;   // no "-0.0"
    if (std::abs (r) < 0.05)
        return "0.0";
    return (r > 0.0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (r), 1);
}

juce::String paramId (int band, const char* what)
{
    return "v" + juce::String (band + 1) + what;
}

void styleLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, theme::ink2);
}

void styleBar (juce::Slider& s)
{
    s.setColour (juce::Slider::trackColourId, theme::blue.withAlpha (0.5f));
}

juce::String bandText (const roomeq::Band& b)
{
    auto t = theme::formatHz (b.freq) + " " + (b.gainDb >= 0.0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92"))
             + juce::String (std::abs (b.gainDb), 1) + " dB";
    if (b.kind == roomeq::BandKind::lowShelf)
        t = "low shelf " + t;
    else if (b.kind == roomeq::BandKind::highShelf)
        t = "high shelf " + t;
    return t;
}
} // namespace

AdaptiveRoomEQEditor::AdaptiveRoomEQEditor (AdaptiveRoomEQProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      captureList ({ [this] (int id)
                     {
                         graph.setData (processor.getEngine().getDisplay(), id);
                         updateQualityCard();
                     },
                     [this] (int id, const juce::String& name) { processor.getEngine().rename (id, name); },
                     [this] (int id, bool include) { processor.getEngine().setExcluded (id, ! include); },
                     [this] (int id) { redo (id); },
                     [this] (int id) { processor.getEngine().remove (id); } })
{
    auto& params = processor.getParameters();

    // ---- Tabs
    for (auto* tab : { &zoneTab, &measureTab, &correctTab, &voicingTab, &loudnessTab, &alignTab, &verifyTab, &monitorTab })
    {
        tab->setLookAndFeel (&tabLook);
        tab->setRadioGroupId (1);
        tab->setClickingTogglesState (true);
        tab->setColour (juce::TextButton::buttonOnColourId, theme::blue.withAlpha (0.6f));
        addChildComponent (tab);
    }
    // A radio button being switched off sends a click too: act only for the one switching on.
    const auto onSelect = [] (juce::Button& b, std::function<void()> f) { b.onClick = [&b, f] { if (b.getToggleState()) f(); }; };
    onSelect (measureTab, [this] { showTab (Tab::measure); });
    onSelect (alignTab, [this] { showTab (Tab::align); });
    onSelect (correctTab, [this] { showTab (Tab::correct); });
    onSelect (verifyTab, [this] { showTab (Tab::verify); });
    onSelect (voicingTab, [this] { showTab (Tab::voicing); });
    onSelect (monitorTab, [this] { showTab (Tab::monitor); });
    onSelect (loudnessTab, [this] { showTab (Tab::loudness); });
    onSelect (zoneTab, [this] { showTab (Tab::zone); });
    {
        // The Tune steps are numbered (the app has no Align step).
        auto step = 1;
        for (auto* t : { &measureTab, &alignTab, &correctTab, &verifyTab })
            if (t != &alignTab || ! processor.isStandalone())
                t->setButtonText (juce::String (step++) + " " + t->getButtonText());
    }

    // Modes.
    for (auto* b : { &setupButton, &tuneButton, &showButton })
    {
        b->setRadioGroupId (4);
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, theme::blue.withAlpha (0.6f));
        addAndMakeVisible (b);
    }
    setupButton.setTooltip ("Setup: the zone, the mic and speaker routing, and the Level Compensation calibration.");
    tuneButton.setTooltip ("Tune: 1 Measure, 2 Align, 3 Correct, 4 Verify.");
    showButton.setTooltip ("Show: the bypasses, the level, the SPL meter and the Engineer Voicing, over the mic's spectrogram.");
    onSelect (setupButton, [this] { setMode (Mode::setup); });
    onSelect (tuneButton, [this] { setMode (Mode::tune); });
    onSelect (showButton, [this] { setMode (Mode::show); });
    showButton.setVisible (! processor.isStandalone());   // the app only plays test signals
    advancedToggle.setTooltip ("Shows the detailed controls and the raw numbers: sweep and smoothing settings, the "
                               "correction's limits, the Level Compensation details, exact SNR and coherence, and each "
                               "capture's impulse response.");
    advancedToggle.onClick = [this] { setAdvanced (advancedToggle.getToggleState()); };
    addAndMakeVisible (advancedToggle);

    const auto combo = [&] (juce::ComboBox& box, juce::Label& label, const juce::String& text, const juce::String& id,
                            std::unique_ptr<ComboAttachment>& attachment, std::vector<juce::Component*>& group)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter (id)))
            box.addItemList (choice->choices, 1);
        attachment = std::make_unique<ComboAttachment> (params, id, box);
        styleLabel (label, text);
        addChildComponent (box);
        addChildComponent (label);
        group.insert (group.end(), { &box, &label });
    };
    const auto slider = [&] (juce::Slider& s, juce::Label& label, const juce::String& text, const juce::String& id,
                             std::unique_ptr<SliderAttachment>& attachment, std::vector<juce::Component*>& group)
    {
        attachment = std::make_unique<SliderAttachment> (params, id, s);
        styleBar (s);
        styleLabel (label, text);
        addChildComponent (s);
        addChildComponent (label);
        group.insert (group.end(), { &s, &label });
    };
    const auto button = [&] (juce::Component& b, std::vector<juce::Component*>& group)
    {
        addChildComponent (b);
        group.push_back (&b);
    };

    // ---- Zone tab
    combo (zone, zoneLabel, "Zone", "zone", zoneAttachment, zoneControls);
    zone.setTooltip ("What this instance drives. Subs are judged on 40-100 Hz, where they play; the others on "
                     "250 Hz-4 kHz. Choosing one sets its starting correction range: subs 20-150 Hz, fills and "
                     "delays from 80 Hz, mains the full range.");
    if (! processor.isStandalone())   // the app only plays test signals
    {
        slider (zoneDelay, zoneDelayLabel, "Delay", "zoneDelay", zoneDelayAttachment, alignControls);
        zoneDelay.setTooltip ("0-300 ms, and the distance sound travels in it. Double-click to type a time, or a "
                              "distance (\"25 m\", \"82 ft\"). Hold Ctrl (Cmd on a Mac) while dragging for fine steps. "
                              "Measurements bypass it; Verify includes it.");
        zoneDelay.setVelocityModeParameters (0.05, 1, 0.0, true, juce::ModifierKeys::ctrlAltCommandModifiers);
        polarityAttachment = std::make_unique<ButtonAttachment> (params, "polarityInvert", polarity);
        polarity.setTooltip ("Flips this output's polarity (for a sub or fill that cancels the mains around the "
                             "crossover). Measurements bypass it; Verify includes it.");
        button (polarity, alignControls);
        alignment = std::make_unique<AlignmentPanel> (processor);
        button (*alignment, alignControls);
    }

    // ---- Measure tab
    combo (signal, signalLabel, "Signal", "measureSignal", signalAttachment, measureControls);
    combo (sweepLength, sweepLengthLabel, "Sweep length", "sweepLength", sweepLengthAttachment, measureControls);
    combo (sweepsPerPosition, sweepsLabel, "Sweeps per position", "sweepsPerPosition", sweepsAttachment, measureControls);
    combo (noiseLength, noiseLengthLabel, "Noise length", "noiseLength", noiseLengthAttachment, measureControls);
    combo (sweepSpeaker, speakerLabel, "Speaker", "sweepSpeaker", speakerAttachment, measureControls);
    combo (smoothing, smoothingLabel, "Smoothing", "smoothing", smoothingAttachment, measureControls);
    slider (sweepLevel, levelLabel, "Level", "sweepLevel", levelAttachment, measureControls);
    signal.setTooltip ("Sweep: quick and rejects speaker distortion. Pink noise: steadier in a noisy room "
                       "(a stray bang is averaged away) but needs 20-30 s.");
    sweepSpeaker.setTooltip ("The speaker the sweep or noise plays on; the other side stays silent.");
    sweepLevel.setTooltip ("Peak level of the sweep or noise. Pink noise sits ~6 dB lower on average at the same setting.");
    advancedControls.insert (advancedControls.end(), { &sweepLength, &sweepLengthLabel, &sweepsPerPosition, &sweepsLabel,
                                                       &noiseLength, &noiseLengthLabel, &smoothing, &smoothingLabel });

    measureButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    measureButton.onClick = [this] { if (micReady()) showResult (processor.startMeasurement()); };
    programButton.setTooltip ("Estimate the response from walk-in music or soundcheck when a sweep isn't possible. "
                              "Uses the plugin's output as the reference; both speakers play.");
    programButton.onClick = [this] { if (micReady()) showResult (processor.startProgram()); };
    stopButton.onClick = [this]
    {
        if (processor.getLoudness().getStep() != LoudnessController::Step::idle)
            processor.getLoudness().cancel();   // also stops calibration noise
        else
            processor.getEngine().cancel();
    };
    clearAllButton.setColour (juce::TextButton::textColourOffId, theme::critical);
    clearAllButton.setTooltip ("Start the room over: forgets every measurement and correction"
                               + juce::String (processor.isStandalone() ? "." : ", and the level calibration."));
    clearAllButton.onClick = [this]
    {
        const auto what = processor.isStandalone()
                              ? juce::String ("Every measurement, and the applied and previous corrections, are deleted.")
                              : juce::String ("Every measurement, the applied and previous corrections and the level "
                                              "calibration are deleted. The correction and level compensation go flat.");
        // Made here rather than in the inner capture: MSVC reads `this` there as the outer lambda.
        juce::Component::SafePointer<AdaptiveRoomEQEditor> editor (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::WarningIcon)
                                          .withTitle ("Clear all room data?")
                                          .withMessage (what + "\n\nThe voicing EQ, targets, mic calibration and settings stay. "
                                                               "This can't be undone.")
                                          .withButton ("Clear all")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [editor] (int result)
                                      {
                                          if (editor != nullptr && result == 1)
                                              editor->showResult (editor->processor.clearRoomData());
                                      });
    };
    button (measureButton, measureControls);
    button (programButton, measureControls);
    button (clearAllButton, measureControls);
    addChildComponent (qualityCard);
    measureControls.push_back (&qualityCard);

    addChildComponent (systemPanel);
    for (auto* b : { &summaryButton, &capturesButton })
    {
        b->setRadioGroupId (3);
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::buttonOnColourId, theme::blue.withAlpha (0.55f));
        addChildComponent (b);
    }
    summaryButton.setToggleState (true, juce::dontSendNotification);
    summaryButton.setTooltip ("The measurements and the proposal in a few lines, and where the correction was held back.");
    capturesButton.setTooltip ("The captured positions, as on the Measure page.");
    summaryButton.onClick = [this]
    {
        showingSummary = summaryButton.getToggleState();
        updateTabVisibility();
    };
    capturesButton.onClick = summaryButton.onClick;
    addAndMakeVisible (stopButton);   // on every tab: Verify runs from the Correct tab

    // ---- Correct tab
    combo (target, targetLabel, "Target", "target", targetAttachment, correctControls);
    target.setTooltip ("The response to correct towards. Custom: drag its points on the graph "
                       "(double-click to add or remove one); save and load them from the Targets menu.");
    targetMenu.onClick = [this] { showTargetMenu(); };
    button (targetMenu, correctControls);
    correctionOnAttachment = std::make_unique<ButtonAttachment> (params, "correctionOn", correctionOn);
    button (correctionOn, correctControls);
    levelMatchAttachment = std::make_unique<ButtonAttachment> (params, "levelMatch", levelMatch);
    levelMatch.setTooltip ("Adds a make-up gain so the correction and voicing leave the music's loudness where it was "
                           "(worked out from their curves, weighted like a LUFS meter). Switching Correction on and "
                           "off is then a level-matched comparison. Level compensation isn't levelled.");
    button (levelMatch, correctControls);
    slider (amount, amountLabel, "Amount", "correctionAmount", amountAttachment, correctControls);
    slider (maxCut, maxCutLabel, "Max cut", "maxCut", maxCutAttachment, correctControls);
    slider (maxBoost, maxBoostLabel, "Max boost", "maxBoost", maxBoostAttachment, correctControls);
    slider (rangeLo, rangeLoLabel, "Correct from", "rangeLo", rangeLoAttachment, correctControls);
    slider (rangeHi, rangeHiLabel, "Correct up to", "rangeHi", rangeHiAttachment, correctControls);
    amount.setTooltip ("Scales every band of the applied correction.");
    sweepLevel.setTextValueSuffix (" dBFS");
    amount.setTextValueSuffix (" %");
    maxCut.setTextValueSuffix (" dB");
    maxBoost.setTextValueSuffix (" dB");
    bandGain.setTextValueSuffix (" dB");
    maxBoost.setTooltip ("Boosts are never placed in nulls or outside the PA's range, whatever this says.");
    advancedControls.insert (advancedControls.end(), { &maxCut, &maxCutLabel, &maxBoost, &maxBoostLabel, &rangeLo, &rangeLoLabel,
                                                       &rangeHi, &rangeHiLabel });

    applyButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    applyButton.setTooltip ("Put the proposed correction on the audio path. The one it replaces is kept for Undo.");
    applyButton.onClick = [this] { processor.getEngine().applyProposal(); };
    compareButton.setClickingTogglesState (true);
    compareButton.setColour (juce::TextButton::buttonOnColourId, theme::orange.withAlpha (0.7f));
    compareButton.setTooltip ("Listen to the previous correction; click again to return to the applied one.");
    compareButton.onClick = [this] { processor.getEngine().setComparingPrevious (compareButton.getToggleState()); };
    undoButton.setTooltip ("Go back to the previous correction (click again to redo).");
    undoButton.onClick = [this] { processor.getEngine().undoApply(); };
    verifyButton.setTooltip ("Measure a position through the correction and voicing EQ, to check the result. "
                             "Verify captures never change the proposal.");
    verifyButton.onClick = [this] { if (micReady()) showResult (processor.startVerify()); };
    exportButton.setTooltip ("The EQ as text to type into a console, or a CSV or JSON file another tool can read: the "
                             "correction as it plays, the voicing, the output level match, the delay and polarity.");
    exportButton.onClick = [this] { showExportMenu(); };
    for (auto* b : { &applyButton, &compareButton, &undoButton, &exportButton })
        button (*b, correctControls);
    verifyButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    button (verifyButton, verifyControls);

    // ---- Voicing tab
    voicingOnAttachment = std::make_unique<ButtonAttachment> (params, "voicingOn", voicingOn);
    button (voicingOn, voicingControls);
    for (int b = 0; b < roomeq::numVoicingBands; ++b)
    {
        auto& bb = bandButtons[static_cast<std::size_t> (b)];
        bb.setButtonText (juce::String (b + 1));
        bb.setRadioGroupId (2);
        bb.setClickingTogglesState (true);
        bb.onClick = [this, b, &bb] { if (bb.getToggleState()) selectVoicingBand (b); };
        button (bb, voicingControls);
    }
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter (paramId (0, "Type"))))
        bandType.addItemList (choice->choices, 1);
    button (bandOn, voicingControls);
    for (auto* c : std::initializer_list<juce::Component*> { &bandType, &bandFreq, &bandGain, &bandQ })
        button (*c, voicingControls);
    for (auto* s : { &bandFreq, &bandGain, &bandQ })
        styleBar (*s);
    styleLabel (bandTypeLabel, "Type");
    styleLabel (bandFreqLabel, "Frequency");
    styleLabel (bandGainLabel, "Gain");
    styleLabel (bandQLabel, "Q");
    for (auto* l : { &bandTypeLabel, &bandFreqLabel, &bandGainLabel, &bandQLabel })
        button (*l, voicingControls);
    graph.onVoicingBandSelected = [this] (int b) { selectVoicingBand (b); };

    // ---- Level compensation tab
    loudOnAttachment = std::make_unique<ButtonAttachment> (params, "loudOn", loudOn);
    loudOn.setTooltip ("Uses equal-loudness behaviour to help keep a similar perceived tonal balance when the playback "
                       "level differs from the calibrated reference level.");
    button (loudOn, loudnessControls);
    slider (loudRef, loudRefLabel, "Reference Level", "loudRef", loudRefAttachment, loudnessControls);
    slider (loudAmount, loudAmountLabel, "Amount", "loudAmount", loudAmountAttachment, loudnessControls);
    loudStrength.addItemList ({ "Subtle", "Natural", "Full" }, 1);
    loudStrength.setTextWhenNothingSelected ("Custom");
    loudStrength.setTooltip ("Presets for Amount: Subtle 50%, Natural 75%, Full 100%. Full theoretical compensation "
                             "doesn't always sound best on mastered music.");
    loudStrength.onChange = [this]
    {
        const auto id = loudStrength.getSelectedId();
        if (auto* param = processor.getParameters().getParameter ("loudAmount"); param != nullptr && id >= 1 && id <= 3)
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (strengthPresets[id - 1]));
            param->endChangeGesture();
        }
    };
    button (loudStrength, loudnessControls);
    slider (loudMaxLow, loudMaxLabel, "Max boost", "loudMaxLow", loudMaxLowAttachment, loudnessControls);
    loudMaxHighAttachment = std::make_unique<SliderAttachment> (params, "loudMaxHigh", loudMaxHigh);
    styleBar (loudMaxHigh);
    button (loudMaxHigh, loudnessControls);
    combo (loudSource, loudSourceLabel, "Level from", "loudSource", loudSourceAttachment, loudnessControls);
    slider (loudSpeed, loudSpeedLabel, "Speed", "loudSpeed", loudSpeedAttachment, loudnessControls);
    loudHighPassAttachment = std::make_unique<ButtonAttachment> (params, "loudHighPass", loudHighPass);
    button (loudHighPass, loudnessControls);
    advancedControls.insert (advancedControls.end(), { &loudMaxLow, &loudMaxHigh, &loudMaxLabel, &loudSource, &loudSourceLabel,
                                                       &loudSpeed, &loudSpeedLabel, &loudHighPass });
    loudRef.setTextValueSuffix (" dB(C)");
    loudAmount.setTextValueSuffix (" %");
    // Two bars share the "Max boost" row, so each says which it is.
    const auto numberIn = [] (const juce::String& text) { return text.retainCharacters ("0123456789.-").getDoubleValue(); };
    loudMaxLow.textFromValueFunction = [] (double v) { return "low " + juce::String (v, 1) + " dB"; };
    loudMaxHigh.textFromValueFunction = [] (double v) { return "high " + juce::String (v, 1) + " dB"; };
    loudMaxLow.valueFromTextFunction = numberIn;
    loudMaxHigh.valueFromTextFunction = numberIn;
    loudMaxLow.updateText();
    loudMaxHigh.updateText();
    loudRef.setTooltip ("The level (dB C at the mix position) where you approved the system's sound. The compensation "
                        "follows how far the current level is below it.");
    loudAmount.setTooltip ("How much of the equal-loudness compensation to apply.");
    loudMaxLow.setTooltip ("Most low-shelf boost, however quiet it gets (never more than 12 dB).");
    loudMaxHigh.setTooltip ("Most high-shelf boost, however quiet it gets.");
    loudSource.setTooltip ("Plugin output: the level is worked out from what the plugin sends (steady, ignores the crowd). "
                           "Mic: from the measurement mic (needs a calibration with the mic connected; only counts "
                           "while the music plays).");
    loudSpeed.setTooltip ("How long the level is averaged over before the EQ follows it, louder or quieter (5 s to "
                          "1 min), so a song's dynamics don't move it. A loud song after a quiet one (6 dB or more, "
                          "lasting a few seconds) is followed within seconds. Pauses between songs are held; changes "
                          "under 2 dB are mostly ignored.");
    loudHighPass.setTooltip ("A 24 dB/octave high-pass at the PA's measured low-end roll-off, rising up to half an "
                             "octave as the bass boost grows, so the boost doesn't push the speakers below their range.");

    calibrateButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    calibrateButton.setTooltip ("Plays pink noise on both speakers through the correction and voicing for 12 s. Read an "
                                "SPL meter at the mix position (C-weighted, slow) while it plays, then enter the reading.");
    calibrateButton.onClick = [this] { showResult (processor.getLoudness().startCalibration()); };
    styleLabel (splLabel, "Meter read");
    splEntry.setInputRestrictions (6, "0123456789.,");
    splEntry.setTextToShowWhenEmpty ("dB(C)", theme::muted);
    splEntry.setJustification (juce::Justification::centredLeft);
    splEntry.onReturnKey = [this] { submitSpl(); };
    setSplButton.onClick = [this] { submitSpl(); };
    styleLabel (calibratorLabel, "Calibrator");
    calibratorLevel.addItem ("94 dB", 1);
    calibratorLevel.addItem ("114 dB", 2);
    calibratorLevel.setSelectedId (processor.getLoudness().getInfo().calibratorSpl > 104.0 ? 2 : 1, juce::dontSendNotification);
    micCalButton.setTooltip ("Optional: with a sound level calibrator on the mic, this measures the mic's sensitivity, "
                             "so the calibration can suggest the SPL the mic heard.");
    micCalButton.onClick = [this]
    {
        if (micReady())
            showResult (processor.getLoudness().startMicCalibration (calibratorLevel.getSelectedId() == 2 ? 114.0 : 94.0));
    };
    recheckButton.setTooltip ("During the show: listens to ~12 s of the music through the mic and compares it with "
                              "the calibration. If the amp gain or a fader after the plugin moved, the calibration "
                              "follows. No test signal plays.");
    recheckButton.onClick = [this] { if (micReady()) showResult (processor.getLoudness().startRecheck()); };
    for (auto* c : std::initializer_list<juce::Component*> { &calibrateButton, &splLabel, &splEntry, &setSplButton,
                                                             &calibratorLabel, &calibratorLevel, &micCalButton, &recheckButton })
        button (*c, loudnessControls);

    addAndMakeVisible (captureList);
    addAndMakeVisible (graph);

    // ---- Show mode's Monitor page (plugin only)
    showRecheckButton.setTooltip ("Listens to ~12 s of the music through the mic and updates the level calibration "
                                  "if the gain after the plugin changed.");
    showRecheckButton.onClick = [this] { if (micReady()) showResult (processor.getLoudness().startRecheck()); };
    splResetButton.setTooltip ("Start the Leqs and the max over.");
    splResetButton.onClick = [this]
    {
        processor.getSpl().reset();
        repaint (splBounds);
    };
    showCorrectionAttachment = std::make_unique<ButtonAttachment> (params, "correctionOn", showCorrectionOn);
    showVoicingAttachment = std::make_unique<ButtonAttachment> (params, "voicingOn", showVoicingOn);
    showLoudAttachment = std::make_unique<ButtonAttachment> (params, "loudOn", showLoudOn);
    showLevelMatchAttachment = std::make_unique<ButtonAttachment> (params, "levelMatch", showLevelMatch);
    for (auto* c : std::initializer_list<juce::Component*> { &showRecheckButton, &splResetButton,
                                                             &showCorrectionOn, &showVoicingOn, &showLoudOn, &showLevelMatch })
    {
        addChildComponent (c);
        showControls.push_back (c);
    }

   #if JucePlugin_Build_Standalone
    if (processor.isStandalone())
    {
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            // JUCE's standalone mutes inputs by default to avoid feedback. The app
            // never sends its input to the speakers, and the mic must be heard.
            holder->getMuteInputValue().setValue (false);
            deviceManager = &holder->deviceManager;
        }
    }
   #endif

    if (processor.isStandalone())
    {
        const auto setUpPicker = [this] (juce::ComboBox& box, juce::Label& label, const juce::String& text, const juce::String& tip)
        {
            styleLabel (label, text);
            box.setTooltip (tip);
            box.setTextWhenNothingSelected ("None");
            box.setTextWhenNoChoicesAvailable ("No audio device");
            box.onChange = [this] { applyDeviceChannels(); };
            addChildComponent (box);
            addChildComponent (label);
            zoneControls.insert (zoneControls.end(), { &box, &label });
        };
        setUpPicker (micInput, micInputLabel, "Mic input", "The interface input the measurement mic is plugged into");
        setUpPicker (speakerOutput, speakerOutputLabel, "Speaker output",
                     "The interface output feeding the speaker to measure. The test signal plays only here.");
        // The speaker is chosen as a physical output instead.
        measureControls.erase (std::remove_if (measureControls.begin(), measureControls.end(),
                                               [this] (juce::Component* c) { return c == &sweepSpeaker || c == &speakerLabel; }),
                               measureControls.end());
        programButton.setTooltip ("Only in the plugin: measuring from music needs the program to pass through it.");
        if (deviceManager != nullptr)
            deviceManager->addChangeListener (this);
        refreshDeviceChannels();
    }

    showingNoiseRows = processor.isNoiseSelected();
    lastMono = processor.isMono();
    selectVoicingBand (0);
    processor.getEngine().addChangeListener (this);
    processor.getLoudness().addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (1060, 740, 2400, 1600);
    setSize (1200, 820);
    advanced = processor.isAdvanced();
    advancedToggle.setToggleState (advanced, juce::dontSendNotification);
    qualityCard.setAdvanced (advanced);
    showTab (Tab::measure);
    setMode (static_cast<Mode> (processor.isStandalone() ? juce::jmin (processor.getUiMode(), 1) : processor.getUiMode()));
    refreshFromEngine();
    startTimerHz (refreshHz);
}

AdaptiveRoomEQEditor::~AdaptiveRoomEQEditor()
{
    if (deviceManager != nullptr)
        deviceManager->removeChangeListener (this);
    processor.getEngine().removeChangeListener (this);
    processor.getLoudness().removeChangeListener (this);
    for (auto* tab : { &zoneTab, &measureTab, &correctTab, &voicingTab, &loudnessTab, &alignTab, &verifyTab, &monitorTab })
        tab->setLookAndFeel (nullptr);
    stopTimer();
}

AdaptiveRoomEQEditor::Mode AdaptiveRoomEQEditor::modeOf (Tab tab)
{
    switch (tab)
    {
        case Tab::zone:
        case Tab::loudness: return Mode::setup;
        case Tab::voicing:
        case Tab::monitor: return Mode::show;
        case Tab::measure:
        case Tab::align:
        case Tab::correct:
        case Tab::verify: break;
    }
    return Mode::tune;
}

std::vector<juce::TextButton*> AdaptiveRoomEQEditor::tabsOf (Mode m)
{
    const auto plugin = ! processor.isStandalone();
    switch (m)
    {
        case Mode::setup:
            return plugin ? std::vector<juce::TextButton*> { &zoneTab, &loudnessTab } : std::vector<juce::TextButton*> { &zoneTab };
        case Mode::show: return { &monitorTab, &voicingTab };
        case Mode::tune: break;
    }
    return plugin ? std::vector<juce::TextButton*> { &measureTab, &alignTab, &correctTab, &verifyTab }
                  : std::vector<juce::TextButton*> { &measureTab, &correctTab, &verifyTab };
}

bool AdaptiveRoomEQEditor::isPageButtonShown (Tab t)
{
    auto& b = t == Tab::measure   ? measureTab
              : t == Tab::correct ? correctTab
              : t == Tab::voicing ? voicingTab
              : t == Tab::zone    ? zoneTab
              : t == Tab::align   ? alignTab
              : t == Tab::verify  ? verifyTab
              : t == Tab::monitor ? monitorTab
                                  : loudnessTab;
    return b.isVisible();
}

void AdaptiveRoomEQEditor::showTab (Tab tab)
{
    if (processor.isStandalone() && (tab == Tab::loudness || tab == Tab::align || modeOf (tab) == Mode::show))
        tab = tab == Tab::loudness ? Tab::zone : Tab::measure;
    currentTab = tab;
    mode = modeOf (tab);
    lastTab[static_cast<std::size_t> (mode)] = tab;
    showViewOn = mode == Mode::show;
    processor.setUiMode (static_cast<int> (mode));
    auto& button = tab == Tab::measure   ? measureTab
                   : tab == Tab::correct ? correctTab
                   : tab == Tab::voicing ? voicingTab
                   : tab == Tab::zone    ? zoneTab
                   : tab == Tab::align   ? alignTab
                   : tab == Tab::verify  ? verifyTab
                   : tab == Tab::monitor ? monitorTab
                                         : loudnessTab;
    button.setToggleState (true, juce::dontSendNotification);
    (mode == Mode::setup ? setupButton : mode == Mode::show ? showButton : tuneButton).setToggleState (true, juce::dontSendNotification);
    updateTabVisibility();
}

void AdaptiveRoomEQEditor::setMode (Mode m)
{
    if (m == Mode::show && processor.isStandalone())
        m = Mode::tune;
    showTab (lastTab[static_cast<std::size_t> (m)]);
}

void AdaptiveRoomEQEditor::setShowView (bool on)
{
    if (on && ! processor.isStandalone())
        showTab (Tab::monitor);
    else
        setMode (Mode::tune);
}

void AdaptiveRoomEQEditor::setAdvanced (bool on)
{
    advanced = on;
    processor.setAdvanced (on);
    advancedToggle.setToggleState (on, juce::dontSendNotification);
    qualityCard.setAdvanced (on);
    updateTabVisibility();
    repaint();
}

void AdaptiveRoomEQEditor::updateTabVisibility()
{
    const auto on = [this] (Tab t) { return currentTab == t; };
    for (auto* c : measureControls)
        c->setVisible (on (Tab::measure));
    for (auto* c : alignControls)
        c->setVisible (on (Tab::align));
    for (auto* c : correctControls)
        c->setVisible (on (Tab::correct));
    for (auto* c : verifyControls)
        c->setVisible (on (Tab::verify));
    for (auto* c : voicingControls)
        c->setVisible (on (Tab::voicing));
    for (auto* c : loudnessControls)
        c->setVisible (on (Tab::loudness));
    for (auto* c : zoneControls)
        c->setVisible (on (Tab::zone));
    for (auto* c : showControls)
        c->setVisible (on (Tab::monitor));
    for (auto* t : { &zoneTab, &measureTab, &correctTab, &voicingTab, &loudnessTab, &alignTab, &verifyTab, &monitorTab })
        t->setVisible (false);
    for (auto* t : tabsOf (mode))
        t->setVisible (true);

    const auto summaryHere = on (Tab::correct);
    summaryButton.setVisible (summaryHere);
    capturesButton.setVisible (summaryHere);
    systemPanel.setVisible (summaryHere && showingSummary);
    captureList.setVisible (! showViewOn && ! (summaryHere && showingSummary));
    graph.setShowMode (showViewOn);
    if (on (Tab::measure))
    {
        for (auto* c : std::initializer_list<juce::Component*> { &sweepLength, &sweepLengthLabel, &sweepsPerPosition, &sweepsLabel })
            c->setVisible (! showingNoiseRows);
        noiseLength.setVisible (showingNoiseRows);
        noiseLengthLabel.setVisible (showingNoiseRows);
        // A mono track has one speaker: nothing to choose.
        sweepSpeaker.setVisible (sweepSpeaker.isVisible() && ! lastMono);
        speakerLabel.setVisible (speakerLabel.isVisible() && ! lastMono);
    }
    if (! advanced)
        for (auto* c : advancedControls)
            c->setVisible (false);
    resized();
    repaint();
}

void AdaptiveRoomEQEditor::updateSignalRows()
{
    updateTabVisibility();
}

void AdaptiveRoomEQEditor::selectVoicingBand (int band)
{
    selectedBand = juce::jlimit (0, roomeq::numVoicingBands - 1, band);
    bandButtons[static_cast<std::size_t> (selectedBand)].setToggleState (true, juce::dontSendNotification);
    auto& params = processor.getParameters();
    bandOnAttachment.reset();
    bandTypeAttachment.reset();
    bandFreqAttachment.reset();
    bandGainAttachment.reset();
    bandQAttachment.reset();
    bandOnAttachment = std::make_unique<ButtonAttachment> (params, paramId (selectedBand, "On"), bandOn);
    bandTypeAttachment = std::make_unique<ComboAttachment> (params, paramId (selectedBand, "Type"), bandType);
    bandFreqAttachment = std::make_unique<SliderAttachment> (params, paramId (selectedBand, "Freq"), bandFreq);
    bandGainAttachment = std::make_unique<SliderAttachment> (params, paramId (selectedBand, "Gain"), bandGain);
    bandQAttachment = std::make_unique<SliderAttachment> (params, paramId (selectedBand, "Q"), bandQ);
    bandOn.setButtonText ("Band " + juce::String (selectedBand + 1) + " on");
    graph.setSelectedVoicingBand (selectedBand);
    repaint();
}

bool AdaptiveRoomEQEditor::micReady()
{
    // Checked before anything plays: a sweep nobody can hear is just noise in the room.
    const auto r = processor.checkMicSignal();
    if (r.wasOk())
        return true;
    errorText = r.getErrorMessage();
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle ("No mic signal")
                                      .withMessage (r.getErrorMessage())
                                      .withButton ("OK")
                                      .withAssociatedComponent (this),
                                  nullptr);
    repaint();
    return false;
}

void AdaptiveRoomEQEditor::showResult (const juce::Result& result)
{
    errorText = result.failed() ? result.getErrorMessage() : juce::String();
    noticeText.clear();
    refreshFromEngine();
}

void AdaptiveRoomEQEditor::redo (int id)
{
    if (! micReady())
        return;
    for (const auto& e : processor.getEngine().getEntries())
        if (e.id == id)
            showResult (e.capture->kind == "program" ? processor.startProgram (id)
                        : e.capture->kind == "noise"  ? processor.startNoise (id)
                                                      : processor.startSweep (id));
}

void AdaptiveRoomEQEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (deviceManager != nullptr && source == deviceManager)
        refreshDeviceChannels();
    else if (source == &processor.getLoudness())
        updateLoudnessControls();
    else
        refreshFromEngine();
}

void AdaptiveRoomEQEditor::refreshDeviceChannels()
{
    const auto fill = [] (juce::ComboBox& box, const juce::StringArray& names, const juce::BigInteger& active,
                          const juce::String& fallback)
    {
        box.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i)
            box.addItem (juce::String (i + 1) + ": " + (names[i].isNotEmpty() ? names[i] : fallback + " " + juce::String (i + 1)), i + 1);
        // Show the first active channel (JUCE may have a stereo pair active by default).
        if (const auto first = active.findNextSetBit (0); first >= 0 && first < names.size())
            box.setSelectedId (first + 1, juce::dontSendNotification);
    };

    auto* device = deviceManager != nullptr ? deviceManager->getCurrentAudioDevice() : nullptr;
    if (device == nullptr)
    {
        fill (micInput, {}, {}, {});
        fill (speakerOutput, {}, {}, {});
        return;
    }
    fill (micInput, device->getInputChannelNames(), device->getActiveInputChannels(), "Input");
    fill (speakerOutput, device->getOutputChannelNames(), device->getActiveOutputChannels(), "Output");
    repaint (headerArea());
}

void AdaptiveRoomEQEditor::applyDeviceChannels()
{
    if (deviceManager == nullptr)
        return;
    auto setup = deviceManager->getAudioDeviceSetup();
    if (const auto mic = micInput.getSelectedId() - 1; mic >= 0)
    {
        setup.useDefaultInputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setBit (mic);
    }
    if (const auto out = speakerOutput.getSelectedId() - 1; out >= 0)
    {
        setup.useDefaultOutputChannels = false;
        setup.outputChannels.clear();
        setup.outputChannels.setBit (out);
    }
    errorText = deviceManager->setAudioDeviceSetup (setup, true);
    refreshDeviceChannels();
}

void AdaptiveRoomEQEditor::refreshFromEngine()
{
    auto& engine = processor.getEngine();
    const auto measuring = engine.getActivity() == MeasurementEngine::Activity::measuring;
    captureList.setEntries (engine.getEntries(), measuring, engine.getAppliedId());
    graph.setData (engine.getDisplay(), captureList.getSelectedId());
    systemPanel.setDisplay (engine.getDisplay());
    updateQualityCard();
    repaint();
}

void AdaptiveRoomEQEditor::updateQualityCard()
{
    std::optional<MeasurementEngine::Entry> selected;
    const auto id = captureList.getSelectedId();
    for (const auto& e : processor.getEngine().getEntries())
        if (e.id == id)
            selected = e;
    qualityCard.setEntry (std::move (selected), processor.getReferenceBand());
}

void AdaptiveRoomEQEditor::timerCallback()
{
    const auto decayed = [] (float current, float peak)
    {
        const auto peakDb = juce::Decibels::gainToDecibels (peak, meterFloorDb);
        return juce::jmax (peakDb, current - meterDecayDbPerTick, meterFloorDb);
    };
    micLevelDb = decayed (micLevelDb, processor.takeMicPeak());
    outputLevelDb = decayed (outputLevelDb, processor.takeOutputPeak());
    if (processor.isMono() != lastMono)   // the host changed the track's layout
    {
        lastMono = processor.isMono();
        updateTabVisibility();
    }
    if (currentTab == Tab::zone || currentTab == Tab::align)
        repaint (infoBounds);

    auto& engine = processor.getEngine();
    const auto activity = engine.getActivity();
    const auto measuring = activity == MeasurementEngine::Activity::measuring;
    const auto loudBusy = processor.getLoudness().getStep() != LoudnessController::Step::idle;
    measureButton.setEnabled (! measuring);
    programButton.setEnabled (! measuring && ! processor.isStandalone());
    clearAllButton.setEnabled (activity == MeasurementEngine::Activity::idle && ! loudBusy);
    stopButton.setEnabled (measuring || loudBusy);
    showRecheckButton.setEnabled (! loudBusy && ! measuring && processor.getLoudness().getInfo().canRecheck);
    splResetButton.setEnabled (processor.getLoudness().getInfo().hasMicOffset);
    micInput.setEnabled (! measuring && micInput.getNumItems() > 0);
    speakerOutput.setEnabled (! measuring && speakerOutput.getNumItems() > 0);
    applyButton.setEnabled (! measuring && engine.canApply());
    compareButton.setEnabled (! measuring && engine.hasPrevious());
    compareButton.setToggleState (engine.isComparingPrevious(), juce::dontSendNotification);
    undoButton.setEnabled (! measuring && engine.hasPrevious());
    verifyButton.setEnabled (! measuring);
    if (measuring)
    {
        errorText.clear();
        noticeText.clear();
    }
    if (processor.isNoiseSelected() != showingNoiseRows)   // also follows host automation
    {
        showingNoiseRows = processor.isNoiseSelected();
        updateSignalRows();
    }

    // Voicing: which bands are on, and which controls apply to the selected type.
    // Band buttons: magenta when the band is on, brighter when selected.
    const auto eq = processor.getEqSettings();
    for (std::size_t b = 0; b < bandButtons.size(); ++b)
    {
        const auto on = eq.voicing[b].on;
        bandButtons[b].setColour (juce::TextButton::buttonColourId, on ? theme::magenta.withAlpha (0.35f) : theme::surface);
        bandButtons[b].setColour (juce::TextButton::buttonOnColourId, on ? theme::magenta : theme::muted);
    }
    const auto& sel = eq.voicing[static_cast<std::size_t> (selectedBand)];
    bandGain.setEnabled (roomeq::hasGain (sel.type));
    bandQ.setEnabled (roomeq::hasQ (sel.type));

    updateLoudnessControls();

    graph.refresh();
    repaint (headerArea());
    repaint (statusBounds);
    if (currentTab == Tab::monitor)
    {
        repaint (showLoudBounds);
        repaint (splBounds);
    }
    else if (currentTab == Tab::correct || currentTab == Tab::loudness || currentTab == Tab::verify)
        repaint (infoBounds);
}

void AdaptiveRoomEQEditor::updateLoudnessControls()
{
    using Step = LoudnessController::Step;
    auto& loud = processor.getLoudness();
    const auto step = loud.getStep();
    const auto measuring = processor.getEngine().getActivity() == MeasurementEngine::Activity::measuring;
    const auto ready = step == Step::idle || step == Step::awaitingSpl;
    const auto info = loud.getInfo();
    calibrateButton.setEnabled (ready && ! measuring);
    splEntry.setEnabled (step == Step::awaitingSpl);
    setSplButton.setEnabled (step == Step::awaitingSpl);
    micCalButton.setEnabled (ready && processor.isMicConnected());
    calibratorLevel.setEnabled (ready);
    recheckButton.setEnabled (step == Step::idle && ! measuring && info.canRecheck);

    // The strength presets follow Amount (host automation included); anything else reads "Custom".
    const auto percent = processor.getLoudnessSettings().config.amount * 100.0;
    auto preset = 0;
    for (int i = 0; i < 3; ++i)
        if (std::abs (percent - static_cast<double> (strengthPresets[i])) < 0.5)
            preset = i + 1;
    if (loudStrength.getSelectedId() != preset)
        loudStrength.setSelectedId (preset, juce::dontSendNotification);

    // A calibration just finished playing: put the mic's suggestion (if any) in the box.
    if (step == Step::awaitingSpl && lastLoudnessStep != Step::awaitingSpl)
    {
        splEntry.setText (info.suggestedSpl ? juce::String (*info.suggestedSpl, 1) : juce::String(), juce::dontSendNotification);
        if (splEntry.isShowing())
            splEntry.grabKeyboardFocus();
    }
    else if (step != Step::awaitingSpl && lastLoudnessStep == Step::awaitingSpl)
    {
        splEntry.clear();
    }
    lastLoudnessStep = step;
}

void AdaptiveRoomEQEditor::drawSpl (juce::Graphics& g, juce::Rectangle<int> area) const
{
    const auto& spl = processor.getSpl();
    const auto info = processor.getLoudness().getInfo();
    auto r = area;
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("SPL at the mic", r.removeFromTop (22), juce::Justification::centredLeft);
    r.removeFromTop (4);
    if (! info.hasMicOffset)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText ("Calibrate the mic with a calibrator (Setup \xe2\x80\xba Level Compensation) to read dB SPL here.", r,
                          juce::Justification::topLeft, 3);
        return;
    }
    const auto offset = info.micOffsetDb;
    const auto reading = [offset] (double dbfs)
    {
        return dbfs > -100.0 ? juce::String (dbfs + offset, 1) : juce::String::fromUTF8 ("\xe2\x80\x94");
    };
    auto big = r.removeFromTop (38);
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    const auto value = reading (spl.fastDb());
    const auto valueWidth = juce::jmin (big.getWidth() / 2,
                                        juce::roundToInt (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), value)) + 8);
    g.drawText (value, big.removeFromLeft (valueWidth), juce::Justification::centredLeft);
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("dB(A) fast", big.removeFromLeft (80).withTrimmedTop (12), juce::Justification::centredLeft);
    g.drawText ("max " + reading (spl.maxDb()), big.withTrimmedTop (12), juce::Justification::centredRight);
    r.removeFromTop (4);
    const auto heard = spl.secondsHeard();
    juce::String leq;
    if (heard == 0)
        leq = "LAeq, LCeq: waiting for the mic";
    else
        leq << "LAeq " << reading (spl.laeqDb()) << "   LCeq " << reading (spl.lceqDb()) << " dB   "
            << (heard >= SplMeter::leqSeconds ? juce::String ("(15 min)")
                                              : "(" + juce::String (heard / 60) + ":" + juce::String (heard % 60).paddedLeft ('0', 2) + ")");
    g.drawText (leq, r.removeFromTop (18), juce::Justification::centredLeft);
}

juce::String AdaptiveRoomEQEditor::showLoudnessText() const
{
    const auto settings = processor.getLoudnessSettings();
    const auto& st = processor.getLoudnessStatus();
    const auto info = processor.getLoudness().getInfo();
    juce::String text = "Level Compensation: ";
    if (! settings.on)
        text << "off.";
    else if (! info.calibrated)
        text << juce::String::fromUTF8 ("not calibrated (Setup \xe2\x80\xba Level Compensation).");
    else if (! st.hasLevel.load())
        text << "waiting for music.";
    else
    {
        const auto delta = static_cast<double> (st.splUsed.load()) - settings.config.referenceSpl;
        text << juce::String (st.splNow.load(), 1) << " dB(C)";
        if (delta < -0.05)
            text << ", " << juce::String (-delta, 1) << " dB under the reference";
        text << "; boost low " << signedDb (st.lowGainDb.load()) << " / high " << signedDb (st.highGainDb.load()) << " dB.";
    }
    text << "\nOutput level match: ";
    if (processor.getEqSettings().levelMatch)
        text << signedDb (processor.getMakeupDb()) << " dB.";
    else
        text << "off.";
    return text;
}

void AdaptiveRoomEQEditor::submitSpl()
{
    const auto text = splEntry.getText().replaceCharacter (',', '.').trim();
    const auto result = text.isEmpty() ? juce::Result::fail ("Enter the level your meter read, in dB(C)")
                                       : processor.getLoudness().setMeasuredSpl (text.getDoubleValue());
    showResult (result);
    updateLoudnessControls();
}

juce::String AdaptiveRoomEQEditor::statusText() const
{
    using Step = LoudnessController::Step;
    if (errorText.isNotEmpty())
        return errorText;
    if (noticeText.isNotEmpty() && juce::Time::getMillisecondCounter() - noticeTime < 5000)
        return noticeText;
    auto& engine = processor.getEngine();
    const auto& loud = processor.getLoudness();
    const auto step = loud.getStep();
    const auto measuring = engine.getActivity() == MeasurementEngine::Activity::measuring;
    if (step == Step::calibrating && measuring)
        return loud.getStatus() + "\n" + engine.getStatus();
    if (step != Step::idle)
        return loud.getStatus();
    if (currentTab == Tab::loudness && engine.getActivity() == MeasurementEngine::Activity::idle && loud.getStatus().isNotEmpty())
        return loud.getStatus();
    return engine.getStatus();
}

juce::String AdaptiveRoomEQEditor::zoneInfo() const
{
    const auto [lo, hi] = processor.getReferenceBand();
    const auto settings = processor.getCorrectionSettings();
    const auto dash = juce::String::fromUTF8 ("\xe2\x80\x93");
    juce::String text;
    text << "Measurements are judged on " << theme::formatHz (lo) << dash << theme::formatHz (hi)
         << (processor.getZone() == AdaptiveRoomEQProcessor::Zone::subs ? ", where subs play" : "")
         << ". Correcting " << theme::formatHz (settings.config.rangeLoHz) << dash << theme::formatHz (settings.config.rangeHiHz)
         << juce::String::fromUTF8 (" (Tune \xe2\x80\xba Correct).\n");
    if (processor.isStandalone())
        return text + "Delay and polarity are set in the plugin in your DAW.";
    if (currentTab == Tab::align)   // the Align page: just what it sets
        text.clear();
    else if (processor.isMono())
        text << "Mono track: the test signal plays on its one speaker.\n";
    else
        text << juce::String::fromUTF8 ("Stereo track: the test signal plays on the speaker chosen in Tune \xe2\x80\xba Measure.\n");
    const auto z = processor.getZoneSettings();
    if (z.delayMs > 0.0 || z.invert)
    {
        if (z.delayMs > 0.0)
            text << "Delayed " << juce::String (zoneDelayText (z.delayMs)) << (z.invert ? ", polarity inverted" : "") << ".";
        else
            text << "Polarity inverted.";
        text << " Measurements bypass this; Verify includes it.";
    }
    return text;
}

juce::String AdaptiveRoomEQEditor::loudnessInfo() const
{
    using Step = LoudnessController::Step;
    const auto& loud = processor.getLoudness();
    const auto info = loud.getInfo();
    const auto settings = processor.getLoudnessSettings();
    const auto& st = processor.getLoudnessStatus();
    const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
    const auto minus = juce::String::fromUTF8 ("\xe2\x88\x92");
    juce::String text;

    if (loud.getStep() == Step::awaitingSpl)
    {
        text << "The noise measured " << juce::String (info.measuredOutputDbfs, 1)
             << " dBFS (C) at the plugin output. Enter what your meter read at the mix position";
        if (info.suggestedSpl)
            text << " (the calibrated mic heard " << juce::String (*info.suggestedSpl, 1) << " dB(C))";
        text << ".\n";
    }

    if (! info.calibrated)
    {
        text << "Not calibrated yet, so the EQ stays flat. Calibrate: play the noise, read an SPL meter at the "
                "mix position (C-weighted, slow) and enter the reading.";
    }
    else
    {
        if (! settings.on)
            text << "Level Compensation is off.\n";
        else if (! st.hasLevel.load())
            text << "Current Level: waiting for music.\n";
        else
        {
            const auto used = static_cast<double> (st.splUsed.load());
            const auto delta = used - settings.config.referenceSpl;
            text << "Current Level " << juce::String (st.splNow.load(), 1) << " dB(C); the EQ follows "
                 << juce::String (used, 1) << ": "
                 << (delta < -0.05 ? minus + juce::String (-delta, 1) + " dB from the reference.\n"
                                   : juce::String ("at the reference or above, no compensation.\n"));
            text << "Boost: low " << signedDb (st.lowGainDb.load()) << " dB (" << theme::formatHz (st.lowFreq.load()) << ")"
                 << dot << "high " << signedDb (st.highGainDb.load()) << " dB (" << theme::formatHz (st.highFreq.load()) << ")";
            if (st.hpFreq.load() > 0.0f)
                text << dot << "high-pass " << theme::formatHz (st.hpFreq.load());
            text << ".\n";
        }
        // The PA's low end limits the bass boost when it starts close to the low shelf.
        const auto limit = loud.getLowLimit(), shelf = loud.getLowShelfHz();
        if (settings.on && limit > 0.0 && shelf > 0.0)
        {
            const auto allowance = roomeq::lowBoostAllowance (limit, shelf);
            const auto most = std::min (settings.config.maxLowDb, settings.config.lowCeilingDb);
            if (allowance < 0.995)
                text << "Bass boost limited to " << juce::String (most * allowance, 1) << " dB: the PA's usable range starts at "
                     << theme::formatHz (limit) << ", close to the low shelf (" << theme::formatHz (shelf) << ").\n";
        }
        if (settings.useMic && ! info.calibration.hasMic)
            text << "Mic tracking needs a calibration with the mic connected; following the plugin output.\n";
        text << "Calibrated " << juce::String (info.calibration.spl, 1) << " dB(C) = "
             << juce::String (info.calibration.outputDbfs, 1) << " dBFS (C) out";
        if (info.calibratedAt > 0)
            text << ", " << juce::Time (info.calibratedAt).formatted ("%d %b %H:%M");
        if (info.recheckedAt > 0)
            text << "; re-checked " << juce::Time (info.recheckedAt).formatted ("%H:%M") << " ("
                 << signedDb (info.recheckChangeDb) << " dB)";
        text << ".";
        if (! info.canRecheck)
            text << " Re-check needs a calibration with the mic connected.";
    }
    if (info.hasMicOffset)
        text << "\nMic: " << juce::String (info.calibratorSpl, 0) << " dB calibrator = "
             << juce::String (info.calibratorSpl - info.micOffsetDb, 1) << " dBFS (C).";
    return text;
}

juce::String AdaptiveRoomEQEditor::summaryLine() const
{
    const auto display = processor.getEngine().getDisplay();
    if (display == nullptr)
        return {};
    const auto& s = display->summary;
    const auto n = s.nGood;
    juce::String text;
    if (n == 0)
        text = "No usable positions yet: redo the captures graded REDO.";
    else if (s.policy.maxCorrectionDb.has_value())
        text << "Quick mode: " << n << " good position" << (n == 1 ? "" : "s") << ", 1/" << s.policy.smoothingFraction
             << "-octave smoothing. Correction capped at " << juce::String (*s.policy.maxCorrectionDb, 0)
             << " dB at " << juce::roundToInt (100.0 * s.policy.strength) << "% strength; measure more positions to strengthen it.";
    else
        text << n << " good positions, 1/" << s.policy.smoothingFraction << "-octave smoothing, full correction strength.";
    text << "   Usable range " << theme::formatHz (s.usable.first) << juce::String::fromUTF8 (" \xe2\x80\x93 ")
         << theme::formatHz (s.usable.second) << ".";
    return text;
}

juce::String AdaptiveRoomEQEditor::verifiedText() const
{
    juce::String text;
    const auto display = processor.getEngine().getDisplay();
    if (display != nullptr && ! display->verifiedDb.empty() && ! display->targetDb.empty() && display->proposal)
    {
        // How close the verified average is to the target over the corrected range (outside nulls).
        const auto& g = display->summary.grid;
        const auto& r = *display->proposal;
        // The fit grid is evenly spaced in log frequency: index its null mask directly.
        const auto perOctave = static_cast<double> (r.grid.size() - 1) / std::log2 (r.grid.back() / r.grid.front());
        double sumSq = 0.0;
        int n = 0;
        for (std::size_t i = 0; i < g.size(); ++i)
        {
            if (g[i] < r.fitRange.first || g[i] > r.fitRange.second || ! std::isfinite (display->verifiedDb[i]))
                continue;
            const auto k = static_cast<std::size_t> (juce::jlimit (0L, static_cast<long> (r.grid.size()) - 1,
                                                                   std::lround (std::log2 (g[i] / r.grid.front()) * perOctave)));
            if (r.nullMask[k])
                continue;
            const auto e = display->verifiedDb[i] - display->targetDb[i];
            sumSq += e * e;
            ++n;
        }
        if (n > 0)
            text << "Verified (" << display->verifiedCount << " position" << (display->verifiedCount == 1 ? "" : "s")
                 << "): " << juce::String (std::sqrt (sumSq / n), 1) << " dB RMS from target.";
    }
    return text;
}

juce::String AdaptiveRoomEQEditor::verifyInfo() const
{
    const auto& engine = processor.getEngine();
    juce::String text;
    if (engine.getAppliedId() == 0)
        text << "Nothing applied yet: apply a System Correction (3 Correct) first, or verify the Engineer Voicing alone.\n";
    else
        text << "Playing: the System Correction (" << static_cast<int> (engine.getApplied().size()) << " band"
             << (engine.getApplied().size() == 1 ? "" : "s") << ", Amount "
             << juce::roundToInt (100.0 * processor.getEqSettings().amount) << "%), the Engineer Voicing"
             << (processor.isStandalone() ? "" : " and the zone's delay") << ".\n";
    const auto verified = verifiedText();
    text << (verified.isNotEmpty() ? verified : juce::String ("Nothing verified yet."));
    return text;
}

juce::String AdaptiveRoomEQEditor::correctionInfo() const
{
    const auto& engine = processor.getEngine();
    const auto display = engine.getDisplay();
    juce::String text;
    const auto dash = juce::String::fromUTF8 (" \xe2\x80\x93 ");
    const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
    if (display == nullptr || ! display->proposal)
    {
        text << "Measure some positions to get a proposed correction.";
    }
    else
    {
        const auto& r = *display->proposal;
        text << "Proposal: " << static_cast<int> (r.bands.size()) << " band" << (r.bands.size() == 1 ? "" : "s");
        if (std::isfinite (r.rmsErrorDb))
            text << ", predicted " << juce::String (r.rmsErrorDb, 1) << " dB RMS from target";
        text << " over " << theme::formatHz (r.fitRange.first) << dash << theme::formatHz (r.fitRange.second) << ".";
        if (r.strength < 1.0)
            text << " Quick mode: " << juce::roundToInt (100.0 * r.strength) << "% strength.";
        text << "\n";
    }

    const auto& applied = engine.getApplied();
    if (engine.getAppliedId() == 0)
        text << "Nothing applied yet.";
    else if (applied.empty())
        text << "Applied: no correction.";
    else
    {
        text << "Applied" << (engine.canApply() ? " (differs from the proposal)" : "") << ": ";
        if (advanced)
        {
            juce::StringArray bands;
            for (const auto& b : applied)
                bands.add (bandText (b));
            text << bands.joinIntoString (dot) << ".";
        }
        else
            text << static_cast<int> (applied.size()) << " band" << (applied.size() == 1 ? "" : "s")
                 << " (Advanced lists them).";
    }
    if (engine.isComparingPrevious())
        text << "\nPlaying the previous correction.";
    if (processor.getEqSettings().levelMatch)
        text << "\nOutput level matched: " << signedDb (processor.getMakeupDb()) << " dB make-up for the correction and voicing.";
    else
        text << "\nOutput level match is off: the EQ changes the level.";

    if (const auto verified = verifiedText(); verified.isNotEmpty())
        text << "\n" << verified;
    return text;
}

void AdaptiveRoomEQEditor::showTargetMenu()
{
    juce::PopupMenu menu;
    const auto isCustom = processor.getTargetChoice() == AdaptiveRoomEQProcessor::targetCustom;
    menu.addItem (juce::String::fromUTF8 ("Save custom target as\xe2\x80\xa6"), isCustom, false, [this] { askToSaveTarget(); });

    juce::PopupMenu load;
    for (const auto& f : processor.getSavedTargets())
        load.addItem (f.getFileNameWithoutExtension(), [this, f] { showResult (processor.loadTarget (f)); });
    if (load.getNumItems() == 0)
        load.addItem ("No saved targets yet", false, false, [] {});
    menu.addSubMenu ("Load saved target", load);
    menu.addSeparator();

    for (int i = 0; i < 3; ++i)
    {
        const auto preset = roomeq::targetPresets()[static_cast<std::size_t> (i)];
        menu.addItem ("Start custom from " + juce::String (preset.name), [this, preset]
        {
            auto t = preset;
            t.name = "Custom";
            processor.setCustomTarget (t);
            if (auto* param = processor.getParameters().getParameter ("target"))
                param->setValueNotifyingHost (param->convertTo0to1 (static_cast<float> (AdaptiveRoomEQProcessor::targetCustom)));
        });
    }
    menu.addSeparator();
    menu.addItem (juce::String::fromUTF8 ("Import target (CSV or JSON)\xe2\x80\xa6"), [this] { chooseTargetFile (true); });
    juce::PopupMenu exportMenu;
    exportMenu.addItem (juce::String::fromUTF8 ("As CSV\xe2\x80\xa6"), [this] { chooseTargetFile (false, ".csv"); });
    exportMenu.addItem (juce::String::fromUTF8 ("As JSON\xe2\x80\xa6"), [this] { chooseTargetFile (false, ".json"); });
    menu.addSubMenu ("Export this target", exportMenu);
    menu.addSeparator();
    menu.addItem ("Show targets folder", [] {
        const auto folder = AdaptiveRoomEQProcessor::getTargetsFolder();
        folder.createDirectory();
        folder.revealToUser();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (targetMenu));
}

void AdaptiveRoomEQEditor::showExportMenu()
{
    juce::PopupMenu menu;
    menu.addItem ("Copy as text (for typing into a console)", [this] { copyEqToClipboard(); });
    menu.addItem (juce::String::fromUTF8 ("Save as CSV\xe2\x80\xa6"), [this] { saveExport (".csv"); });
    menu.addItem (juce::String::fromUTF8 ("Save as JSON\xe2\x80\xa6"), [this] { saveExport (".json"); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (exportButton));
}

void AdaptiveRoomEQEditor::copyEqToClipboard()
{
    juce::SystemClipboard::copyTextToClipboard (juce::String::fromUTF8 (roomeq::eqExportText (processor.getEqExport()).c_str()));
    noticeText = "EQ copied as text.";
    noticeTime = juce::Time::getMillisecondCounter();
    repaint();
}

void AdaptiveRoomEQEditor::saveExport (const juce::String& extension)
{
    const auto name = juce::File::createLegalFileName (processor.getZoneLabel() + " EQ" + extension);
    exportChooser = std::make_unique<juce::FileChooser> ("Export the EQ",
                                                         juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (name),
                                                         "*" + extension);
    juce::Component::SafePointer<AdaptiveRoomEQEditor> safe (this);
    exportChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::warnAboutOverwriting,
                                [safe, extension] (const juce::FileChooser& chooser)
                                {
                                    if (safe == nullptr || chooser.getResult() == juce::File())
                                        return;
                                    const auto file = chooser.getResult().withFileExtension (extension);
                                    const auto result = safe->processor.exportEq (file);
                                    safe->showResult (result);
                                    if (result.wasOk())
                                    {
                                        safe->noticeText = "EQ exported to " + file.getFileName() + ".";
                                        safe->noticeTime = juce::Time::getMillisecondCounter();
                                        safe->repaint();
                                    }
                                });
}

void AdaptiveRoomEQEditor::chooseTargetFile (bool importing, const juce::String& extension)
{
    const auto name = juce::File::createLegalFileName (juce::String::fromUTF8 (processor.getTarget().name.c_str()) + " target" + extension);
    const auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    targetChooser = std::make_unique<juce::FileChooser> (importing ? "Import a target" : "Export the target",
                                                         importing ? start : start.getChildFile (name),
                                                         importing ? "*.csv;*.txt;*.json" : "*" + extension);
    const auto flags = importing ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                 : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                       | juce::FileBrowserComponent::warnAboutOverwriting;
    juce::Component::SafePointer<AdaptiveRoomEQEditor> safe (this);
    targetChooser->launchAsync (flags, [safe, importing, extension] (const juce::FileChooser& chooser)
    {
        if (safe == nullptr || chooser.getResult() == juce::File())
            return;
        if (importing)
        {
            const auto file = chooser.getResult();
            const auto result = safe->processor.importTarget (file);
            safe->showResult (result);
            if (result.wasOk())
            {
                safe->noticeText = "Target imported from " + file.getFileName() + " into Custom.";
                safe->noticeTime = juce::Time::getMillisecondCounter();
            }
        }
        else
        {
            const auto file = chooser.getResult().withFileExtension (extension);
            const auto result = safe->processor.exportTarget (file);
            safe->showResult (result);
            if (result.wasOk())
            {
                safe->noticeText = "Target exported to " + file.getFileName() + ".";
                safe->noticeTime = juce::Time::getMillisecondCounter();
            }
        }
        safe->repaint();
    });
}

void AdaptiveRoomEQEditor::askToSaveTarget()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save custom target", "Name for this target:", juce::MessageBoxIconType::NoIcon);
    saveDialog->addTextEditor ("name", juce::String::fromUTF8 (processor.getCustomTarget().name.c_str()));
    saveDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<AdaptiveRoomEQEditor> (this)] (int result)
    {
        if (safe == nullptr || safe->saveDialog == nullptr)
            return;
        const auto name = safe->saveDialog->getTextEditorContents ("name");
        if (result == 1)
        {
            const auto r = safe->processor.saveCustomTarget (name);
            safe->errorText = r.failed() ? r.getErrorMessage() : juce::String();
        }
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->saveDialog.reset(); });
    }), false);
}

juce::Rectangle<int> AdaptiveRoomEQEditor::headerArea() const
{
    return getLocalBounds().removeFromTop (headerHeight);
}

juce::Rectangle<int> AdaptiveRoomEQEditor::controlsArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).removeFromLeft (controlsWidth);
}

juce::Rectangle<int> AdaptiveRoomEQEditor::summaryArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).withTrimmedLeft (controlsWidth + margin).removeFromBottom (22);
}

void AdaptiveRoomEQEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::surface);

    // Header: title, mic status and meters.
    auto header = headerArea().reduced (margin, 10);
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText (JucePlugin_Name, header.removeFromLeft (modeButtonsX), juce::Justification::centredLeft);
    header.setLeft (advancedToggle.getRight() + 16);   // the mode buttons and Advanced

    // The meters, narrower in a small window (the mic's routing text goes first).
    const auto micConnected = processor.isMicConnected();
    const auto meterWidth = juce::jlimit (170, 270, (header.getWidth() - 16 - (micConnected ? 0 : 240)) / 2);
    auto meters = header.removeFromRight (2 * meterWidth + 16);
    drawMeter (g, meters.removeFromRight (meterWidth).withSizeKeepingCentre (meterWidth, 18), "Output", outputLevelDb);
    meters.removeFromRight (16);
    drawMeter (g, meters.removeFromRight (meterWidth).withSizeKeepingCentre (meterWidth, 18), "Mic", micLevelDb, true);

    g.setFont (juce::FontOptions (13.0f));
    g.setColour (micConnected ? theme::ink2 : theme::warning);
    auto micText = ! micConnected ? juce::String ("! Mic not connected: route the measurement mic to the sidechain input")
                                  : juce::String ("Measurement mic on the sidechain input");
    if (processor.isStandalone())
        micText = speakerOutput.getNumItems() == 0 ? juce::String ("! No audio device: open Options > Audio/MIDI Settings")
                  : "Mic: " + micInput.getText() + "     Test signal plays on: " + speakerOutput.getText();
    if (header.getWidth() >= 140)
        g.drawFittedText (micText, header.withTrimmedRight (12), juce::Justification::centredLeft, 2, 0.8f);

    // Controls panel.
    const auto controls = controlsArea();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (controls.toFloat(), 6.0f);

    // Status: progress bar + text (every tab).
    auto status = statusBounds.reduced (12, 0);
    auto& engine = processor.getEngine();
    const auto activity = engine.getActivity();
    g.setColour (theme::axis);
    g.drawHorizontalLine (statusBounds.getY() - 8, static_cast<float> (controls.getX() + 12), static_cast<float> (controls.getRight() - 12));
    auto bar = status.removeFromTop (8).toFloat();
    g.setColour (theme::grid);
    g.fillRoundedRectangle (bar, 3.0f);
    const auto& loud = processor.getLoudness();
    const auto loudRecording = loud.getStep() == LoudnessController::Step::micCalibrating
                               || loud.getStep() == LoudnessController::Step::rechecking;
    if (loud.isAnalysing())
    {
        const auto phase = static_cast<float> (juce::Time::getMillisecondCounter() % 1200) / 1200.0f;
        g.setColour (theme::gold.withAlpha (0.8f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * 0.25f).withX (bar.getX() + bar.getWidth() * 0.75f * phase), 3.0f);
    }
    else if (loudRecording)
    {
        g.setColour (theme::gold);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, loud.getProgress())), 3.0f);
    }
    else if (activity == MeasurementEngine::Activity::measuring)
    {
        g.setColour (theme::blue);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, engine.getProgress())), 3.0f);
    }
    else if (activity == MeasurementEngine::Activity::analysing)
    {
        const auto phase = static_cast<float> (juce::Time::getMillisecondCounter() % 1200) / 1200.0f;
        g.setColour (theme::blue.withAlpha (0.7f));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * 0.25f).withX (bar.getX() + bar.getWidth() * 0.75f * phase), 3.0f);
    }
    status.removeFromTop (8);
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (errorText.isNotEmpty() ? theme::critical : theme::ink2);
    g.drawFittedText (statusText(), status.withTrimmedRight (80), juce::Justification::topLeft, 4);

    if (currentTab == Tab::monitor)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (showLoudnessText(), showLoudBounds, juce::Justification::topLeft,
                          juce::jmax (1, showLoudBounds.getHeight() / 15), 0.9f);
        g.setColour (theme::axis);
        g.drawHorizontalLine (splRuleY, static_cast<float> (controls.getX() + 12), static_cast<float> (controls.getRight() - 12));
        drawSpl (g, splBounds);
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (13.0f));
        g.drawFittedText (summaryLine(), summaryArea(), juce::Justification::centredLeft, 1);
        return;
    }

    // Page text: correction details, and a tip.
    if (currentTab == Tab::correct || currentTab == Tab::verify)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (currentTab == Tab::correct ? correctionInfo() : verifyInfo(), infoBounds, juce::Justification::topLeft,
                          juce::jmax (1, infoBounds.getHeight() / 15), 0.9f);
    }
    else if (currentTab == Tab::zone || currentTab == Tab::align)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (zoneInfo(), infoBounds, juce::Justification::topLeft, juce::jmax (1, infoBounds.getHeight() / 15), 0.9f);
    }
    else if (currentTab == Tab::loudness)
    {
        g.setColour (theme::axis);
        g.drawHorizontalLine (calibrationRuleY, static_cast<float> (controls.getX() + 12), static_cast<float> (controls.getRight() - 12));
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (loudnessInfo(), infoBounds, juce::Justification::topLeft, juce::jmax (1, infoBounds.getHeight() / 15), 0.9f);
    }
    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (12.0f));
    const auto* tip = currentTab == Tab::measure   ? placementTip
                      : currentTab == Tab::correct ? correctTip
                      : currentTab == Tab::voicing ? voicingTip
                      : currentTab == Tab::zone    ? zoneTip
                      : currentTab == Tab::align   ? alignTip
                      : currentTab == Tab::verify  ? verifyTip
                                                   : loudnessTip;
    if (! tipBounds.isEmpty())
        g.drawFittedText (tip, tipBounds, juce::Justification::bottomLeft, 6);

    // Quick-mode / usable-range line under the graph.
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (summaryLine(), summaryArea(), juce::Justification::centredLeft, 1);
}

void AdaptiveRoomEQEditor::drawMeter (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, float levelDb,
                                      bool withTarget) const
{
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (label, area.removeFromLeft (52), juce::Justification::centredLeft);
    g.drawText (levelDb <= meterFloorDb ? juce::String ("-inf") : juce::String (levelDb, 1) + " dBFS",
                area.removeFromRight (78), juce::Justification::centredRight);

    const auto bar = area.reduced (0, 3).toFloat();
    g.setColour (theme::grid);
    g.fillRoundedRectangle (bar, 3.0f);
    const auto fraction = juce::jlimit (0.0f, 1.0f, juce::jmap (levelDb, meterFloorDb, 0.0f, 0.0f, 1.0f));
    g.setColour (levelDb > -3.0f ? theme::critical : theme::blue);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * fraction), 3.0f);

    if (withTarget)   // the zone the mic's peaks should sit in, over the bar so it shows either way
    {
        const auto xFor = [&bar] (float db) { return bar.getX() + bar.getWidth() * juce::jmap (db, meterFloorDb, 0.0f, 0.0f, 1.0f); };
        const auto lo = xFor (micTargetLowDb), hi = xFor (micTargetHighDb);
        g.setColour (theme::good.withAlpha (0.3f));
        g.fillRect (juce::Rectangle<float> (lo, bar.getY(), hi - lo, bar.getHeight()));
        g.setColour (theme::good);
        g.fillRect (juce::Rectangle<float> (lo, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f));
        g.fillRect (juce::Rectangle<float> (hi - 1.5f, bar.getY() - 2.0f, 1.5f, bar.getHeight() + 4.0f));
    }
}

void AdaptiveRoomEQEditor::resized()
{
    {
        // Setup / Tune / Show and Advanced, after the title.
        auto x = margin + modeButtonsX;
        const auto y = (headerHeight - 30) / 2;
        for (auto* b : { &setupButton, &tuneButton, &showButton })
        {
            if (! b->isVisible())
                continue;
            const auto w = b == &setupButton ? 66 : 60;
            b->setBounds (x, y, w, 30);
            x += w + 2;
        }
        advancedToggle.setBounds (x + 10, y, 100, 30);
    }
    auto panel = controlsArea().reduced (12);

    // Tab bar: this mode's pages, each as wide as its word plus an equal share of what's left.
    auto tabs = panel.removeFromTop (30);
    {
        const auto shown = tabsOf (mode);
        const juce::Font font { juce::FontOptions (TabLook::tabFontHeight) };
        std::vector<float> widths;
        auto total = 0.0f;
        for (auto* t : shown)
            total += widths.emplace_back (juce::GlyphArrangement::getStringWidth (font, t->getButtonText()));
        const auto share = (static_cast<float> (tabs.getWidth()) - total) / static_cast<float> (shown.size());
        auto x = static_cast<float> (tabs.getX());
        for (std::size_t i = 0; i < shown.size(); ++i)
        {
            const auto right = i + 1 == shown.size() ? static_cast<float> (tabs.getRight()) : x + widths[i] + share;
            shown[i]->setBounds (juce::Rectangle<int> (juce::roundToInt (x), tabs.getY(), juce::roundToInt (right) - juce::roundToInt (x),
                                                       tabs.getHeight()).reduced (1, 0));
            x = right;
        }
    }
    panel.removeFromTop (14);

    // Status at the bottom, with Stop beside it.
    statusBounds = panel.removeFromBottom (statusHeight).expanded (12, 0);
    stopButton.setBounds (statusBounds.getRight() - 12 - 70, statusBounds.getY() + 16, 70, 28);
    panel.removeFromBottom (16);

    if (currentTab == Tab::monitor)
    {
        // The bypasses (two by two), the level re-check and readout, then the SPL meter.
        for (auto pair : { std::pair { &showCorrectionOn, &showLevelMatch }, std::pair { &showVoicingOn, &showLoudOn } })
        {
            auto r = panel.removeFromTop (24);
            pair.first->setBounds (r.removeFromLeft (r.getWidth() / 2));
            pair.second->setBounds (r);
            panel.removeFromTop (6);
        }
        panel.removeFromTop (4);
        showRecheckButton.setBounds (panel.removeFromTop (30));
        panel.removeFromTop (10);
        showLoudBounds = panel.removeFromTop (juce::jmin (panel.getHeight(), 34));
        splRuleY = panel.getY() + 6;
        panel.removeFromTop (16);
        splBounds = panel;
        splResetButton.setBounds (splBounds.getRight() - 60, splBounds.getY(), 60, 22);
        tipBounds = infoBounds = {};
    }
    else
    {
    auto content = panel;
    const auto row = [&] (juce::Label& label, juce::Component& control)
    {
        if (! control.isVisible())   // Advanced only, or not for this track
            return;
        auto r = content.removeFromTop (28);
        label.setBounds (r.removeFromLeft (120));
        control.setBounds (r);
        content.removeFromTop (8);
    };
    const auto fullRow = [&] (juce::Component& control, int height)
    {
        if (! control.isVisible())
            return;
        control.setBounds (content.removeFromTop (height));
        content.removeFromTop (8);
    };

    switch (currentTab)
    {
        case Tab::measure:
        {
            row (signalLabel, signal);
            if (showingNoiseRows)
                row (noiseLengthLabel, noiseLength);
            else
            {
                row (sweepLengthLabel, sweepLength);
                row (sweepsLabel, sweepsPerPosition);
            }
            if (! processor.isStandalone() && ! lastMono)
                row (speakerLabel, sweepSpeaker);
            row (levelLabel, sweepLevel);
            row (smoothingLabel, smoothing);
            content.removeFromTop (6);
            fullRow (measureButton, 38);
            fullRow (programButton, 30);
            fullRow (clearAllButton, 26);
            break;
        }
        case Tab::correct:
        {
            {
                auto r = content.removeFromTop (28);
                targetLabel.setBounds (r.removeFromLeft (120));
                targetMenu.setBounds (r.removeFromRight (84));
                r.removeFromRight (6);
                target.setBounds (r);
                content.removeFromTop (8);
            }
            {
                auto r = content.removeFromTop (24);
                correctionOn.setBounds (r.removeFromLeft (r.getWidth() / 2));
                levelMatch.setBounds (r);
                content.removeFromTop (8);
            }
            row (amountLabel, amount);
            row (maxCutLabel, maxCut);
            row (maxBoostLabel, maxBoost);
            row (rangeLoLabel, rangeLo);
            row (rangeHiLabel, rangeHi);
            content.removeFromTop (4);
            fullRow (applyButton, 36);
            {
                auto r = content.removeFromTop (28);
                undoButton.setBounds (r.removeFromRight (90));
                r.removeFromRight (8);
                compareButton.setBounds (r);
                content.removeFromTop (8);
            }
            fullRow (exportButton, 28);
            break;
        }
        case Tab::voicing:
        {
            fullRow (voicingOn, 24);
            {
                auto r = content.removeFromTop (30);
                const auto w = r.getWidth() / roomeq::numVoicingBands;
                for (auto& b : bandButtons)
                    b.setBounds (r.removeFromLeft (w).reduced (2, 0));
                content.removeFromTop (12);
            }
            fullRow (bandOn, 24);
            row (bandTypeLabel, bandType);
            row (bandFreqLabel, bandFreq);
            row (bandGainLabel, bandGain);
            row (bandQLabel, bandQ);
            break;
        }
        case Tab::loudness:
        {
            // Tighter rows: this tab has the most controls.
            const auto tightRow = [&] (juce::Label& label, juce::Component& control)
            {
                if (! control.isVisible())
                    return;
                auto r = content.removeFromTop (26);
                label.setBounds (r.removeFromLeft (120));
                control.setBounds (r);
                content.removeFromTop (6);
            };
            fullRow (loudOn, 22);
            tightRow (loudRefLabel, loudRef);
            {
                auto r = content.removeFromTop (26);
                loudAmountLabel.setBounds (r.removeFromLeft (120));
                loudStrength.setBounds (r.removeFromLeft (92));
                r.removeFromLeft (6);
                loudAmount.setBounds (r);
                content.removeFromTop (6);
            }
            if (loudMaxLow.isVisible())
            {
                auto r = content.removeFromTop (26);
                loudMaxLabel.setBounds (r.removeFromLeft (120));
                loudMaxLow.setBounds (r.removeFromLeft (r.getWidth() / 2).withTrimmedRight (3));
                loudMaxHigh.setBounds (r.withTrimmedLeft (3));
                content.removeFromTop (6);
            }
            tightRow (loudSourceLabel, loudSource);
            tightRow (loudSpeedLabel, loudSpeed);
            fullRow (loudHighPass, 22);
            calibrationRuleY = content.getY() + 1;
            content.removeFromTop (10);
            fullRow (calibrateButton, 30);
            {
                auto r = content.removeFromTop (26);
                splLabel.setBounds (r.removeFromLeft (120));
                setSplButton.setBounds (r.removeFromRight (60));
                r.removeFromRight (6);
                splEntry.setBounds (r);
                content.removeFromTop (6);
            }
            {
                auto r = content.removeFromTop (26);
                calibratorLabel.setBounds (r.removeFromLeft (120));
                micCalButton.setBounds (r.removeFromRight (104));
                r.removeFromRight (6);
                calibratorLevel.setBounds (r);
                content.removeFromTop (8);
            }
            fullRow (recheckButton, 30);
            break;
        }
        case Tab::zone:
        {
            if (processor.isStandalone())
            {
                row (micInputLabel, micInput);
                row (speakerOutputLabel, speakerOutput);
                content.removeFromTop (6);
            }
            row (zoneLabel, zone);
            break;
        }
        case Tab::align:
        {
            if (alignment != nullptr)
            {
                row (zoneDelayLabel, zoneDelay);
                fullRow (polarity, 24);
                fullRow (*alignment, AlignmentPanel::preferredHeight);
            }
            break;
        }
        case Tab::verify:
        {
            fullRow (verifyButton, 38);
            break;
        }
        case Tab::monitor:
            break;
    }

    // What's left: correction / loudness details and the page's tip. When the
    // window is too small for both, the details stay and the tip goes.
    const auto tipHeight = currentTab == Tab::loudness ? 64 : 84;
    const auto detailsHeight = currentTab == Tab::correct || currentTab == Tab::loudness || currentTab == Tab::zone
                                       || currentTab == Tab::align || currentTab == Tab::verify
                                   ? 90
                               : currentTab == Tab::measure                                                         ? QualityCard::preferredHeight
                                                                                                                    : 0;
    tipBounds = content.getHeight() >= tipHeight + detailsHeight ? content.removeFromBottom (tipHeight) : juce::Rectangle<int>();
    infoBounds = content.withTrimmedTop (4);
    if (currentTab == Tab::measure)
    {
        qualityCard.setBounds (infoBounds.withTrimmedTop (4).withTrimmedBottom (6));
        qualityCard.setVisible (! showViewOn && qualityCard.getHeight() >= QualityCard::minimumHeight);
    }
    }

    // Right: the capture list (setup view) and the graph.
    auto right = getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).withTrimmedLeft (controlsWidth + margin);
    right.removeFromBottom (30);   // summary line
    if (! showViewOn)
    {
        auto top = right.removeFromTop (juce::jmin (4 * 56 + 4, right.getHeight() / 3));
        if (currentTab == Tab::correct)
        {
            // The same space, with a switch between the summary and the list.
            auto strip = top.removeFromTop (26);
            summaryButton.setBounds (strip.removeFromLeft (140));
            strip.removeFromLeft (4);
            capturesButton.setBounds (strip.removeFromLeft (100));
            top.removeFromTop (4);
        }
        captureList.setBounds (top);
        systemPanel.setBounds (top);
        right.removeFromTop (12);
    }
    graph.setBounds (right);
}
