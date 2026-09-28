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
constexpr int controlsWidth = 300;
constexpr int headerHeight = 60;

const char* const placementTip =
    "Measure 3-5 spots across the audience area, at different distances and off-axis. "
    "Avoid symmetric spots on the centre line. The sweep plays on one speaker; the "
    "correction will be applied to both sides.";
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
    const auto setUpCombo = [&] (juce::ComboBox& box, juce::Label& label, const juce::String& text, const juce::String& paramId,
                                 std::unique_ptr<ComboAttachment>& attachment)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (params.getParameter (paramId)))
            box.addItemList (choice->choices, 1);
        attachment = std::make_unique<ComboAttachment> (params, paramId, box);
        label.setText (text, juce::dontSendNotification);
        label.setColour (juce::Label::textColourId, theme::ink2);
        addAndMakeVisible (box);
        addAndMakeVisible (label);
    };
    setUpCombo (sweepLength, sweepLengthLabel, "Sweep length", "sweepLength", sweepLengthAttachment);
    setUpCombo (sweepsPerPosition, sweepsLabel, "Sweeps per position", "sweepsPerPosition", sweepsAttachment);
    setUpCombo (sweepSpeaker, speakerLabel, "Sweep speaker", "sweepSpeaker", speakerAttachment);
    setUpCombo (smoothing, smoothingLabel, "Smoothing", "smoothing", smoothingAttachment);

    levelAttachment = std::make_unique<SliderAttachment> (params, "sweepLevel", sweepLevel);
    sweepLevel.setTextValueSuffix (" dBFS");
    sweepLevel.setColour (juce::Slider::trackColourId, theme::blue.withAlpha (0.5f));
    levelLabel.setText ("Sweep level", juce::dontSendNotification);
    levelLabel.setColour (juce::Label::textColourId, theme::ink2);
    addAndMakeVisible (sweepLevel);
    addAndMakeVisible (levelLabel);

    measureButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    measureButton.onClick = [this] { showResult (processor.startSweep()); };
    programButton.setTooltip ("Estimate the response from walk-in music or soundcheck when a sweep isn't possible. "
                              "Uses the plugin input as the reference; both speakers play.");
    programButton.onClick = [this] { showResult (processor.startProgram()); };
    stopButton.onClick = [this] { processor.getEngine().cancel(); };
    addAndMakeVisible (measureButton);
    addAndMakeVisible (programButton);
    addAndMakeVisible (stopButton);

    addAndMakeVisible (captureList);
    addAndMakeVisible (graph);

   #if JucePlugin_Build_Standalone
    // JUCE's standalone mutes inputs by default to avoid feedback, which would
    // also silence the measurement mic.
    if (processor.isStandalone())
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            holder->getMuteInputValue().setValue (false);
   #endif

    processor.getEngine().addChangeListener (this);
    refreshFromEngine();

    setResizable (true, true);
    setResizeLimits (1000, 640, 2400, 1600);
    setSize (1160, 760);
    startTimerHz (refreshHz);
}

AdaptiveRoomEQEditor::~AdaptiveRoomEQEditor()
{
    processor.getEngine().removeChangeListener (this);
    stopTimer();
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
            showResult (e.capture->kind == "program" ? processor.startProgram (id) : processor.startSweep (id));
}

void AdaptiveRoomEQEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromEngine();
}

void AdaptiveRoomEQEditor::refreshFromEngine()
{
    auto& engine = processor.getEngine();
    const auto measuring = engine.getActivity() == MeasurementEngine::Activity::measuring;
    captureList.setEntries (engine.getEntries(), measuring);
    graph.setData (engine.getDisplay(), captureList.getSelectedId());
    repaint (statusArea().getUnion (summaryArea()));
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

    const auto activity = processor.getEngine().getActivity();
    const auto measuring = activity == MeasurementEngine::Activity::measuring;
    measureButton.setEnabled (! measuring);
    programButton.setEnabled (! measuring);
    stopButton.setEnabled (measuring);
    if (measuring)
        errorText.clear();

    repaint (headerArea());
    repaint (statusArea());
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
             << "-octave smoothing. Correction will be capped at " << juce::String (*s.policy.maxCorrectionDb, 0)
             << " dB at " << juce::roundToInt (100.0 * s.policy.strength) << "% strength; measure more positions to strengthen it.";
    else
        text << n << " good positions, 1/" << s.policy.smoothingFraction << "-octave smoothing, full correction strength.";
    text << "   Usable range " << theme::formatHz (s.usable.first) << juce::String::fromUTF8 (" \xe2\x80\x93 ")
         << theme::formatHz (s.usable.second) << ".";
    return text;
}

juce::Rectangle<int> AdaptiveRoomEQEditor::headerArea() const
{
    return getLocalBounds().removeFromTop (headerHeight);
}

juce::Rectangle<int> AdaptiveRoomEQEditor::controlsArea() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).removeFromLeft (controlsWidth);
}

juce::Rectangle<int> AdaptiveRoomEQEditor::statusArea() const
{
    return controlsArea().withTrimmedTop (350).withHeight (96);
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
    const auto micText = ! micConnected ? juce::String ("! Mic not connected: route the measurement mic to the sidechain input")
                         : processor.isStandalone() ? juce::String ("Inputs 1-2: program   Input 3: measurement mic")
                                                    : juce::String ("Measurement mic on the sidechain input");
    g.drawText (micText, header, juce::Justification::centredLeft);

    // Controls panel.
    const auto controls = controlsArea();
    g.setColour (theme::panel);
    g.fillRoundedRectangle (controls.toFloat(), 6.0f);

    // Status: progress bar + text.
    auto status = statusArea().reduced (12, 0);
    auto& engine = processor.getEngine();
    const auto activity = engine.getActivity();
    auto bar = status.removeFromTop (8).toFloat();
    g.setColour (theme::grid);
    g.fillRoundedRectangle (bar, 3.0f);
    if (activity == MeasurementEngine::Activity::measuring)
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
    g.drawFittedText (errorText.isNotEmpty() ? errorText : engine.getStatus(), status, juce::Justification::topLeft, 4);

    // Placement tip.
    auto tip = controls.reduced (12).removeFromBottom (110);
    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (12.5f));
    g.drawFittedText (placementTip, tip, juce::Justification::bottomLeft, 7);

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
    auto controls = controlsArea().reduced (12);
    const auto row = [&] (juce::Label& label, juce::Component& control)
    {
        auto r = controls.removeFromTop (28);
        label.setBounds (r.removeFromLeft (132));
        control.setBounds (r);
        controls.removeFromTop (8);
    };
    row (sweepLengthLabel, sweepLength);
    row (sweepsLabel, sweepsPerPosition);
    row (speakerLabel, sweepSpeaker);
    row (levelLabel, sweepLevel);
    row (smoothingLabel, smoothing);

    controls.removeFromTop (8);
    measureButton.setBounds (controls.removeFromTop (38));
    controls.removeFromTop (8);
    auto buttons = controls.removeFromTop (30);
    stopButton.setBounds (buttons.removeFromRight (70));
    buttons.removeFromRight (8);
    programButton.setBounds (buttons);

    auto right = getLocalBounds().withTrimmedTop (headerHeight).reduced (margin).withTrimmedLeft (controlsWidth + margin);
    right.removeFromBottom (30);   // summary line
    captureList.setBounds (right.removeFromTop (juce::jmin (5 * 56 + 4, right.getHeight() / 2)));   // five rows: 3-5 positions
    right.removeFromTop (12);
    graph.setBounds (right);
}
