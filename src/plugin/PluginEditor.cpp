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
constexpr int statusHeight = 92;

const char* const placementTip =
    "Measure 3-5 spots across the audience area, at different distances and off-axis. "
    "Avoid symmetric spots on the centre line. The signal plays on one speaker; the "
    "correction is applied to both sides.";

const char* const correctTip =
    "Measurements play straight to the speaker, so the fit always sees the PA's own response. "
    "Apply puts the proposal on the audio path; Verify measures a position through the EQ "
    "(violet on the graph).";

const char* const voicingTip =
    "Your taste layer after the correction; measuring never changes it. On the graph, drag a "
    "numbered handle: sideways for frequency, up and down for gain. The mouse wheel sets Q; "
    "double-click switches a band on or off.";

const char* const loudnessTip =
    "Keeps the balance you hear at the reference level as the show gets quieter: bass and treble come up "
    "as the level drops (ISO 226). Turn the volume down before the plugin. Calibrate once per setup; "
    "re-check if the amp gain changes.";

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
      captureList ({ [this] (int id) { graph.setData (processor.getEngine().getDisplay(), id); },
                     [this] (int id, const juce::String& name) { processor.getEngine().rename (id, name); },
                     [this] (int id, bool include) { processor.getEngine().setExcluded (id, ! include); },
                     [this] (int id) { redo (id); },
                     [this] (int id) { processor.getEngine().remove (id); } })
{
    auto& params = processor.getParameters();

    // ---- Tabs
    for (auto* tab : { &measureTab, &correctTab, &voicingTab, &loudnessTab })
    {
        tab->setRadioGroupId (1);
        tab->setClickingTogglesState (true);
        tab->setColour (juce::TextButton::buttonOnColourId, theme::blue.withAlpha (0.6f));
        addAndMakeVisible (tab);
    }
    loudnessTab.setVisible (! processor.isStandalone());   // the app has no program to compensate
    measureTab.onClick = [this] { showTab (Tab::measure); };
    correctTab.onClick = [this] { showTab (Tab::correct); };
    voicingTab.onClick = [this] { showTab (Tab::voicing); };
    loudnessTab.onClick = [this] { showTab (Tab::loudness); };

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

    measureButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    measureButton.onClick = [this] { showResult (processor.startMeasurement()); };
    programButton.setTooltip ("Estimate the response from walk-in music or soundcheck when a sweep isn't possible. "
                              "Uses the plugin's output as the reference; both speakers play.");
    programButton.onClick = [this] { showResult (processor.startProgram()); };
    stopButton.onClick = [this]
    {
        if (processor.getLoudness().getStep() != LoudnessController::Step::idle)
            processor.getLoudness().cancel();   // also stops calibration noise
        else
            processor.getEngine().cancel();
    };
    button (measureButton, measureControls);
    button (programButton, measureControls);
    addAndMakeVisible (stopButton);   // on every tab: Verify runs from the Correct tab

    // ---- Correct tab
    combo (target, targetLabel, "Target", "target", targetAttachment, correctControls);
    target.setTooltip ("The response to correct towards. Custom: drag its points on the graph "
                       "(double-click to add or remove one); save and load them from the Targets menu.");
    targetMenu.onClick = [this] { showTargetMenu(); };
    button (targetMenu, correctControls);
    correctionOnAttachment = std::make_unique<ButtonAttachment> (params, "correctionOn", correctionOn);
    button (correctionOn, correctControls);
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
    verifyButton.onClick = [this] { showResult (processor.startVerify()); };
    for (auto* b : { &applyButton, &compareButton, &undoButton, &verifyButton })
        button (*b, correctControls);

    // ---- Voicing tab
    voicingOnAttachment = std::make_unique<ButtonAttachment> (params, "voicingOn", voicingOn);
    button (voicingOn, voicingControls);
    for (int b = 0; b < roomeq::numVoicingBands; ++b)
    {
        auto& bb = bandButtons[static_cast<std::size_t> (b)];
        bb.setButtonText (juce::String (b + 1));
        bb.setRadioGroupId (2);
        bb.setClickingTogglesState (true);
        bb.onClick = [this, b] { selectVoicingBand (b); };
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

    // ---- Loudness tab
    loudOnAttachment = std::make_unique<ButtonAttachment> (params, "loudOn", loudOn);
    button (loudOn, loudnessControls);
    slider (loudRef, loudRefLabel, "Reference", "loudRef", loudRefAttachment, loudnessControls);
    slider (loudAmount, loudAmountLabel, "Amount", "loudAmount", loudAmountAttachment, loudnessControls);
    slider (loudMaxLow, loudMaxLabel, "Max boost", "loudMaxLow", loudMaxLowAttachment, loudnessControls);
    loudMaxHighAttachment = std::make_unique<SliderAttachment> (params, "loudMaxHigh", loudMaxHigh);
    styleBar (loudMaxHigh);
    button (loudMaxHigh, loudnessControls);
    combo (loudSource, loudSourceLabel, "Level from", "loudSource", loudSourceAttachment, loudnessControls);
    slider (loudSpeed, loudSpeedLabel, "Speed", "loudSpeed", loudSpeedAttachment, loudnessControls);
    loudHighPassAttachment = std::make_unique<ButtonAttachment> (params, "loudHighPass", loudHighPass);
    button (loudHighPass, loudnessControls);
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
    loudRef.setTooltip ("The level (dB C at the mix position) where the mix sounds right with no compensation. "
                        "Below it, bass and treble are raised to keep that balance.");
    loudAmount.setTooltip ("How much of the ISO 226 compensation to apply.");
    loudMaxLow.setTooltip ("Most low-shelf boost, however quiet it gets (never more than 12 dB).");
    loudMaxHigh.setTooltip ("Most high-shelf boost, however quiet it gets.");
    loudSource.setTooltip ("Plugin output: the level is worked out from what the plugin sends (steady, ignores the crowd). "
                           "Mic: from the measurement mic (needs a calibration with the mic connected; only counts "
                           "while the music plays).");
    loudSpeed.setTooltip ("How quickly the EQ follows a lower level. Louder is followed within about a second; "
                          "pauses between songs are held. Changes under 2 dB are mostly ignored.");
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
    { showResult (processor.getLoudness().startMicCalibration (calibratorLevel.getSelectedId() == 2 ? 114.0 : 94.0)); };
    recheckButton.setTooltip ("During the show: listens to ~12 s of the music through the mic and compares it with "
                              "the calibration. If the amp gain or a fader after the plugin moved, the calibration "
                              "follows. No test signal plays.");
    recheckButton.onClick = [this] { showResult (processor.getLoudness().startRecheck()); };
    for (auto* c : std::initializer_list<juce::Component*> { &calibrateButton, &splLabel, &splEntry, &setSplButton,
                                                             &calibratorLabel, &calibratorLevel, &micCalButton, &recheckButton })
        button (*c, loudnessControls);

    addAndMakeVisible (captureList);
    addAndMakeVisible (graph);

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
            measureControls.insert (measureControls.end(), { &box, &label });
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
    selectVoicingBand (0);
    processor.getEngine().addChangeListener (this);
    processor.getLoudness().addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (1060, 740, 2400, 1600);
    setSize (1200, 820);
    showTab (Tab::measure);
    refreshFromEngine();
    startTimerHz (refreshHz);
}

AdaptiveRoomEQEditor::~AdaptiveRoomEQEditor()
{
    if (deviceManager != nullptr)
        deviceManager->removeChangeListener (this);
    processor.getEngine().removeChangeListener (this);
    processor.getLoudness().removeChangeListener (this);
    stopTimer();
}

void AdaptiveRoomEQEditor::showTab (Tab tab)
{
    if (tab == Tab::loudness && processor.isStandalone())
        tab = Tab::measure;
    currentTab = tab;
    auto& button = tab == Tab::measure ? measureTab : tab == Tab::correct ? correctTab : tab == Tab::voicing ? voicingTab : loudnessTab;
    button.setToggleState (true, juce::dontSendNotification);
    updateTabVisibility();
}

void AdaptiveRoomEQEditor::updateTabVisibility()
{
    for (auto* c : measureControls)
        c->setVisible (currentTab == Tab::measure);
    for (auto* c : correctControls)
        c->setVisible (currentTab == Tab::correct);
    for (auto* c : voicingControls)
        c->setVisible (currentTab == Tab::voicing);
    for (auto* c : loudnessControls)
        c->setVisible (currentTab == Tab::loudness);
    if (currentTab == Tab::measure)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &sweepLength, &sweepLengthLabel, &sweepsPerPosition, &sweepsLabel })
            c->setVisible (! showingNoiseRows);
        noiseLength.setVisible (showingNoiseRows);
        noiseLengthLabel.setVisible (showingNoiseRows);
    }
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

void AdaptiveRoomEQEditor::showResult (const juce::Result& result)
{
    errorText = result.failed() ? result.getErrorMessage() : juce::String();
    refreshFromEngine();
}

void AdaptiveRoomEQEditor::redo (int id)
{
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
    repaint();
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

    auto& engine = processor.getEngine();
    const auto activity = engine.getActivity();
    const auto measuring = activity == MeasurementEngine::Activity::measuring;
    const auto loudBusy = processor.getLoudness().getStep() != LoudnessController::Step::idle;
    measureButton.setEnabled (! measuring);
    programButton.setEnabled (! measuring && ! processor.isStandalone());
    stopButton.setEnabled (measuring || loudBusy);
    micInput.setEnabled (! measuring && micInput.getNumItems() > 0);
    speakerOutput.setEnabled (! measuring && speakerOutput.getNumItems() > 0);
    applyButton.setEnabled (! measuring && engine.canApply());
    compareButton.setEnabled (! measuring && engine.hasPrevious());
    compareButton.setToggleState (engine.isComparingPrevious(), juce::dontSendNotification);
    undoButton.setEnabled (! measuring && engine.hasPrevious());
    verifyButton.setEnabled (! measuring);
    if (measuring)
        errorText.clear();
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
    if (currentTab == Tab::correct || currentTab == Tab::loudness)
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

juce::String AdaptiveRoomEQEditor::loudnessInfo() const
{
    using Step = LoudnessController::Step;
    const auto& loud = processor.getLoudness();
    const auto info = loud.getInfo();
    const auto settings = processor.getLoudnessSettings();
    const auto& st = processor.getLoudnessStatus();
    const auto dot = juce::String::fromUTF8 (" \xc2\xb7 ");
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
            text << "Loudness compensation is off.\n";
        else if (! st.hasLevel.load())
            text << "Level: waiting for music.\n";
        else
        {
            const auto used = static_cast<double> (st.splUsed.load());
            const auto below = settings.config.referenceSpl - used;
            text << "Level " << juce::String (st.splNow.load(), 1) << " dB(C), EQ at " << juce::String (used, 1)
                 << (below > 0.05 ? " (" + juce::String (below, 1) + " below the reference).\n" : " (at the reference or above).\n");
            text << "Boost: low " << signedDb (st.lowGainDb.load()) << " dB (" << theme::formatHz (st.lowFreq.load()) << ")"
                 << dot << "high " << signedDb (st.highGainDb.load()) << " dB (" << theme::formatHz (st.highFreq.load()) << ")";
            if (st.hpFreq.load() > 0.0f)
                text << dot << "high-pass " << theme::formatHz (st.hpFreq.load());
            text << ".\n";
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
        juce::StringArray bands;
        for (const auto& b : applied)
            bands.add (bandText (b));
        text << bands.joinIntoString (dot) << ".";
    }
    if (engine.isComparingPrevious())
        text << "\nPlaying the previous correction.";

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
            text << "\nVerified (" << display->verifiedCount << " position" << (display->verifiedCount == 1 ? "" : "s")
                 << "): " << juce::String (std::sqrt (sumSq / n), 1) << " dB RMS from target.";
    }
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
    menu.addItem ("Show targets folder", [] {
        const auto folder = AdaptiveRoomEQProcessor::getTargetsFolder();
        folder.createDirectory();
        folder.revealToUser();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (targetMenu));
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
    g.drawText (JucePlugin_Name, header.removeFromLeft (260), juce::Justification::centredLeft);

    auto meters = header.removeFromRight (560);
    drawMeter (g, meters.removeFromRight (270).withSizeKeepingCentre (270, 18), "Output", outputLevelDb);
    meters.removeFromRight (16);
    drawMeter (g, meters.removeFromRight (270).withSizeKeepingCentre (270, 18), "Mic", micLevelDb);

    const auto micConnected = processor.isMicConnected();
    g.setFont (juce::FontOptions (13.0f));
    g.setColour (micConnected ? theme::ink2 : theme::warning);
    auto micText = ! micConnected ? juce::String ("! Mic not connected: route the measurement mic to the sidechain input")
                                  : juce::String ("Measurement mic on the sidechain input");
    if (processor.isStandalone())
        micText = speakerOutput.getNumItems() == 0 ? juce::String ("! No audio device: open Options > Audio/MIDI Settings")
                  : "Mic: " + micInput.getText() + "     Test signal plays on: " + speakerOutput.getText();
    g.drawText (micText, header, juce::Justification::centredLeft);

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

    // Tab text: correction details, and a tip.
    if (currentTab == Tab::correct)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (correctionInfo(), infoBounds, juce::Justification::topLeft, 12, 0.9f);
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
                                                   : loudnessTip;
    if (! tipBounds.isEmpty())
        g.drawFittedText (tip, tipBounds, juce::Justification::bottomLeft, 6);

    // Quick-mode / usable-range line under the graph.
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (summaryLine(), summaryArea(), juce::Justification::centredLeft, 1);
}

void AdaptiveRoomEQEditor::drawMeter (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& label, float levelDb) const
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
}

void AdaptiveRoomEQEditor::resized()
{
    auto panel = controlsArea().reduced (12);

    // Tab bar.
    auto tabs = panel.removeFromTop (30);
    const auto tabWidth = tabs.getWidth() / (processor.isStandalone() ? 3 : 4);
    measureTab.setBounds (tabs.removeFromLeft (tabWidth).reduced (1, 0));
    correctTab.setBounds (tabs.removeFromLeft (tabWidth).reduced (1, 0));
    voicingTab.setBounds (tabs.removeFromLeft (tabWidth).reduced (1, 0));
    loudnessTab.setBounds (tabs.reduced (1, 0));
    panel.removeFromTop (14);

    // Status at the bottom, with Stop beside it.
    statusBounds = panel.removeFromBottom (statusHeight).expanded (12, 0);
    stopButton.setBounds (statusBounds.getRight() - 12 - 70, statusBounds.getY() + 16, 70, 28);
    panel.removeFromBottom (16);

    auto content = panel;
    const auto row = [&] (juce::Label& label, juce::Component& control)
    {
        auto r = content.removeFromTop (28);
        label.setBounds (r.removeFromLeft (120));
        control.setBounds (r);
        content.removeFromTop (8);
    };
    const auto fullRow = [&] (juce::Component& control, int height)
    {
        control.setBounds (content.removeFromTop (height));
        content.removeFromTop (8);
    };

    switch (currentTab)
    {
        case Tab::measure:
        {
            if (processor.isStandalone())
            {
                row (micInputLabel, micInput);
                row (speakerOutputLabel, speakerOutput);
            }
            row (signalLabel, signal);
            if (showingNoiseRows)
                row (noiseLengthLabel, noiseLength);
            else
            {
                row (sweepLengthLabel, sweepLength);
                row (sweepsLabel, sweepsPerPosition);
            }
            if (! processor.isStandalone())
                row (speakerLabel, sweepSpeaker);
            row (levelLabel, sweepLevel);
            row (smoothingLabel, smoothing);
            content.removeFromTop (6);
            fullRow (measureButton, 38);
            fullRow (programButton, 30);
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
            fullRow (correctionOn, 24);
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
            fullRow (verifyButton, 30);
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
                auto r = content.removeFromTop (26);
                label.setBounds (r.removeFromLeft (120));
                control.setBounds (r);
                content.removeFromTop (6);
            };
            fullRow (loudOn, 22);
            tightRow (loudRefLabel, loudRef);
            tightRow (loudAmountLabel, loudAmount);
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
    }

    // What's left: correction / loudness details and the tab's tip (the Loudness
    // tab keeps its details and drops the tip when the window is small).
    if (currentTab == Tab::loudness)
        tipBounds = content.getHeight() >= 150 ? content.removeFromBottom (64) : juce::Rectangle<int>();
    else
        tipBounds = content.removeFromBottom (84);
    infoBounds = content.withTrimmedTop (4);

    auto right = getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).withTrimmedLeft (controlsWidth + margin);
    right.removeFromBottom (30);   // summary line
    captureList.setBounds (right.removeFromTop (juce::jmin (4 * 56 + 4, right.getHeight() / 3)));
    right.removeFromTop (12);
    graph.setBounds (right);
}
