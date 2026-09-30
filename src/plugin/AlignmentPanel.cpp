#include "plugin/AlignmentPanel.h"

#include "plugin/Theme.h"

namespace
{
void styleLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, theme::ink2);
}

juce::String ms2 (double ms)
{
    return juce::String (ms, 2) + " ms";
}

juce::String measurementText (const juce::String& name, const roomeq::Arrival& a)
{
    return name + juce::String::fromUTF8 ("  \xc2\xb7  ") + ms2 (a.ms) + juce::String::fromUTF8 ("  \xc2\xb7  ")
           + roomeq::confidenceLabel (a.confidence);
}

// Subs line up with sweeps: pink noise and music don't keep the phase.
bool hasPhase (const std::shared_ptr<const roomeq::Capture>& c)
{
    return c != nullptr && ! c->low.empty();
}

juce::String db1 (double db)
{
    return juce::String (db, 1) + " dB";
}

double numberIn (const juce::String& text)
{
    return text.replaceCharacter (',', '.').retainCharacters ("0123456789.").getDoubleValue();
}
} // namespace

AlignmentPanel::AlignmentPanel (AdaptiveRoomEQProcessor& p) : processor (p)
{
    heading.setText ("Alignment", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    heading.setColour (juce::Label::textColourId, theme::ink);
    styleLabel (latencyLabel, "System latency");
    styleLabel (withLabel, "Line up with");
    styleLabel (theirLabel, "Their arrival");
    styleLabel (ownLabel, "This zone's");
    for (auto* c : std::initializer_list<juce::Component*> { &heading, &latencyLabel, &withLabel, &theirLabel, &ownLabel,
                                                             &latencyValue, &loopbackButton, &withBox, &theirBox, &typedArrival,
                                                             &ownBox, &applyButton })
        addAndMakeVisible (c);

    latencyValue.setInputRestrictions (8, "0123456789.,");
    latencyValue.setTextToShowWhenEmpty ("not measured", theme::muted);
    latencyValue.setJustification (juce::Justification::centredLeft);
    latencyValue.setTooltip ("The interface and host round trip in ms, taken off arrivals to give flight times (and the "
                             "equivalent distances). Measure it with Loopback, or type it. Alignment between zones on the "
                             "same interface doesn't need it.");
    const auto setLatency = [this]
    {
        const auto text = latencyValue.getText().trim();
        processor.getEngine().setSystemLatencyMs (text.isEmpty() ? std::nullopt : std::optional<double> (numberIn (text)));
        latencyValue.giveAwayKeyboardFocus();
        refresh();
    };
    latencyValue.onReturnKey = setLatency;
    latencyValue.onFocusLost = setLatency;

    loopbackButton.setTooltip ("Measures the system latency: patch the output the test signal plays on straight into the mic "
                               "input with a cable, then press Measure. Nothing is filed as a capture.");
    loopbackButton.onClick = [this]
    {
        juce::Component::SafePointer<AlignmentPanel> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                          .withTitle ("Measure the system latency")
                                          .withMessage ("Patch the output the test signal plays on straight into the mic input "
                                                        "with a cable (no mic, no speaker), then press Measure. A sweep plays "
                                                        "through the cable.")
                                          .withButton ("Measure")
                                          .withButton ("Cancel")
                                          .withAssociatedComponent (this),
                                      [safe] (int button)
                                      {
                                          if (safe != nullptr && button == 1)
                                              safe->processor.startLatencyMeasurement();
                                      });
    };

    withBox.setTooltip ("The main system's instance of the plugin in this DAW (its zone and track), or type its arrival "
                        "if it runs somewhere this one can't see.");
    theirBox.setTooltip ("The main system's measurement, from the same mic spot as this zone's.");
    ownBox.setTooltip ("This zone's measurement, from the spot where it and the main system overlap.");
    typedArrival.setInputRestrictions (8, "0123456789.,");
    typedArrival.setTextToShowWhenEmpty ("main arrival, ms", theme::muted);
    typedArrival.setTooltip ("The main system's arrival at the same spot, as its own measurement showed it (loop delay, "
                             "without its zone delay).");
    for (auto* box : { &withBox, &theirBox, &ownBox })
        box->onChange = [this] { update(); };
    withBox.onChange = [this]
    {
        rebuildLists();
        update();
    };
    typedArrival.onTextChange = [this] { update(); };

    applyButton.setColour (juce::TextButton::buttonColourId, theme::blue);
    applyButton.setTooltip ("Sets this zone's Delay (and a sub's polarity) to the suggestion. Nothing changes until you press it.");
    applyButton.onClick = [this]
    {
        if (subMode())
        {
            if (subSuggestion && subSuggestion->ok && subSuggestion->delayMs >= 0.0)
            {
                processor.setZoneDelayMs (subSuggestion->delayMs);
                processor.setZonePolarity (subSuggestion->invert);
            }
        }
        else if (suggestion && suggestion->delayMs > 0.0)
        {
            processor.setZoneDelayMs (suggestion->delayMs);
        }
        update();
    };

    refresh();
    startTimerHz (4);
}

AlignmentPanel::~AlignmentPanel()
{
    stopTimer();
}

void AlignmentPanel::refresh()
{
    if (! latencyValue.hasKeyboardFocus (true))
    {
        const auto latency = processor.getEngine().getSystemLatencyMs();
        const auto text = latency ? juce::String (*latency, 2) : juce::String();
        if (latencyValue.getText() != text)
            latencyValue.setText (text, juce::dontSendNotification);
    }

    // Rebuild the lists only when what's in them changed, keeping the selections.
    juce::String fingerprint (subMode() ? "subs|" : "arrivals|");
    for (const auto& z : processor.getZoneRegistry().others (processor.getInstanceId()))
    {
        fingerprint << z.instance.toString() << z.label << "|" << z.delayMs << "|" << static_cast<int> (z.invert) << "|"
                    << (z.latencyMs ? juce::String (*z.latencyMs) : "-") << "|";
        for (const auto& m : z.measurements)
            fingerprint << m.id << ":" << m.arrival.ms << ",";
    }
    fingerprint << "#";
    for (const auto& e : processor.getEngine().getEntries())
        if (! e.verify)
            fingerprint << e.id << ":" << e.arrival.ms << juce::String::fromUTF8 (e.capture->name.c_str()) << ",";
    if (fingerprint != listedFingerprint)
    {
        listedFingerprint = fingerprint;
        rebuildLists();
    }
    update();
}

void AlignmentPanel::rebuildLists()
{
    // Other zones: Mains first, zones with measurements before empty ones; then the typed option.
    const auto previousZone = withBox.getSelectedId() >= 1 && withBox.getSelectedId() <= static_cast<int> (zones.size())
                                  ? std::optional<juce::Uuid> (zones[static_cast<std::size_t> (withBox.getSelectedId() - 1)].instance)
                                  : std::nullopt;
    const auto wasTyped = withBox.getSelectedId() == typedId() && typedId() > 0;
    const auto subs = subMode();
    theirLabel.setText (subs ? "Their sweep" : "Their arrival", juce::dontSendNotification);
    ownLabel.setText (subs ? "This sub's" : "This zone's", juce::dontSendNotification);
    withBox.setTooltip (subs ? "The mains' instance of the plugin in this DAW (its zone and track)."
                             : "The main system's instance of the plugin in this DAW (its zone and track), or type its "
                               "arrival if it runs somewhere this one can't see.");
    theirBox.setTooltip (subs ? "The mains' sweep, from the same mic spot as the sub's: near where they cross over."
                              : "The main system's measurement, from the same mic spot as this zone's.");
    ownBox.setTooltip (subs ? "The sub's sweep, from the same spot as the mains' one."
                            : "This zone's measurement, from the spot where it and the main system overlap.");
    zones = processor.getZoneRegistry().others (processor.getInstanceId());
    const auto rank = [] (const ZoneRegistry::Zone& z) { return (z.measurements.empty() ? 2 : 0) + (z.zone == 0 ? 0 : 1); };
    std::stable_sort (zones.begin(), zones.end(), [&] (const auto& a, const auto& b) { return rank (a) < rank (b); });
    withBox.clear (juce::dontSendNotification);
    for (std::size_t i = 0; i < zones.size(); ++i)
        withBox.addItem (zones[i].label, static_cast<int> (i) + 1);
    if (! subs)
        withBox.addItem ("Type the main arrival", typedId());
    withBox.setTextWhenNoChoicesAvailable ("No mains instance in this DAW");
    auto pick = zones.empty() ? (subs ? 0 : typedId()) : 1;
    for (std::size_t i = 0; i < zones.size(); ++i)
        if (previousZone && zones[i].instance == *previousZone)
            pick = static_cast<int> (i) + 1;
    if (wasTyped)
        pick = typedId();
    withBox.setSelectedId (pick, juce::dontSendNotification);

    // Their measurements, newest first (keeping the one picked).
    const auto previousTheir = theirBox.getSelectedId();
    theirBox.clear (juce::dontSendNotification);
    if (pick >= 1 && pick <= static_cast<int> (zones.size()))
    {
        const auto& z = zones[static_cast<std::size_t> (pick - 1)];
        for (auto it = z.measurements.rbegin(); it != z.measurements.rend(); ++it)
            if (! subs || hasPhase (it->capture))
                theirBox.addItem (subs ? it->name : measurementText (it->name, it->arrival), it->id);
        if (theirBox.indexOfItemId (previousTheir) >= 0)
            theirBox.setSelectedId (previousTheir, juce::dontSendNotification);
        else if (theirBox.getNumItems() > 0)
            theirBox.setSelectedItemIndex (0, juce::dontSendNotification);
    }
    theirBox.setTextWhenNoChoicesAvailable (subs ? "No sweeps there yet" : "No measurements there yet");

    // This zone's measurements, newest first.
    const auto previousOwn = ownBox.getSelectedId();
    own.clear();
    for (const auto& e : processor.getEngine().getEntries())
        if (! e.verify && (! subs || hasPhase (e.capture)))
            own.insert (own.begin(), e);
    ownBox.clear (juce::dontSendNotification);
    for (const auto& e : own)
    {
        const auto name = juce::String::fromUTF8 (e.capture->name.c_str());
        ownBox.addItem (subs ? name : measurementText (name, e.arrival), e.id);
    }
    if (ownBox.indexOfItemId (previousOwn) >= 0)
        ownBox.setSelectedId (previousOwn, juce::dontSendNotification);
    else if (ownBox.getNumItems() > 0)
        ownBox.setSelectedItemIndex (0, juce::dontSendNotification);
    ownBox.setTextWhenNoChoicesAvailable (subs ? "Sweep the sub first" : "Measure this zone first");

    const auto typed = pick == typedId();
    theirBox.setVisible (! typed);
    typedArrival.setVisible (typed);
}

void AlignmentPanel::update()
{
    suggestion.reset();
    result.clear();
    if (subMode())
    {
        updateSub();
        return;
    }
    subSuggestion.reset();
    subInputs.clear();
    const auto withId = withBox.getSelectedId();
    const auto typed = withId == typedId();

    // The main system's arrival.
    std::optional<double> mainMs, mainLatency;
    auto mainDelay = 0.0;
    auto mainConfidence = roomeq::Confidence::high;
    juce::String mainName;
    if (typed)
    {
        if (typedArrival.getText().trim().isNotEmpty())
        {
            mainMs = numberIn (typedArrival.getText());
            mainName = "Main (typed)";
        }
    }
    else if (withId >= 1 && withId <= static_cast<int> (zones.size()))
    {
        const auto& z = zones[static_cast<std::size_t> (withId - 1)];
        for (const auto& m : z.measurements)
            if (m.id == theirBox.getSelectedId())
            {
                mainMs = m.arrival.ms;
                mainConfidence = m.arrival.confidence;
                mainName = z.label + " " + m.name;
            }
        mainLatency = z.latencyMs;
        mainDelay = z.delayMs;
    }

    // This zone's.
    const MeasurementEngine::Entry* mine = nullptr;
    for (const auto& e : own)
        if (e.id == ownBox.getSelectedId())
            mine = &e;

    if (! mainMs || mine == nullptr)
    {
        result = "Measure this zone where it and the main system overlap, and pick the main system's measurement from "
                 "the same spot (or type its arrival).";
        applyButton.setButtonText ("Apply");
        applyButton.setEnabled (false);
        repaint();
        return;
    }

    const auto ownLatency = processor.getEngine().getSystemLatencyMs();
    suggestion = suggestDelay (*mainMs, mainLatency, mainDelay, mine->arrival.ms, ownLatency);
    const auto& s = *suggestion;
    const auto weaker = static_cast<int> (mainConfidence) >= static_cast<int> (mine->arrival.confidence) ? mainConfidence
                                                                                                         : mine->arrival.confidence;
    result << mainName << ": " << ms2 (*mainMs) << ";  this zone " << juce::String::fromUTF8 (mine->capture->name.c_str())
           << ": " << ms2 (mine->arrival.ms) << ".\n";
    if (s.note.empty())
    {
        const auto metres = s.delayMs * 1e-3 * roomeq::speedOfSound;
        result << "Suggested delay: " << ms2 (s.delayMs) << " (" << juce::String (metres, 2) << " m / "
               << juce::String (metres / 0.3048, 1) << " ft)";
        if (mainDelay > 0.0)
            result << ", with the main system's own " << ms2 (mainDelay);
        if (mainLatency && ownLatency)
            result << ", system latencies taken off";
        result << ".\n";
    }
    else
    {
        result << juce::String::fromUTF8 (s.note.c_str()) << ".\n";
    }
    result << "Confidence: " << roomeq::confidenceLabel (weaker) << ".";

    const auto current = processor.getZoneSettings().delayMs;
    applyButton.setButtonText ("Apply " + ms2 (s.delayMs));
    applyButton.setEnabled (s.delayMs > 0.0 && std::abs (s.delayMs - current) > 0.005);
    repaint();
}

void AlignmentPanel::updateSub()
{
    // The mains' sweep and this sub's.
    const ZoneRegistry::Zone* mains = nullptr;
    std::shared_ptr<const roomeq::Capture> theirs;
    const auto withId = withBox.getSelectedId();
    if (withId >= 1 && withId <= static_cast<int> (zones.size()))
    {
        mains = &zones[static_cast<std::size_t> (withId - 1)];
        for (const auto& m : mains->measurements)
            if (m.id == theirBox.getSelectedId() && hasPhase (m.capture))
                theirs = m.capture;
    }
    std::shared_ptr<const roomeq::Capture> mine;
    for (const auto& e : own)
        if (e.id == ownBox.getSelectedId())
            mine = e.capture;

    if (mains == nullptr || theirs == nullptr || mine == nullptr)
    {
        subSuggestion.reset();
        subInputs.clear();
        result = "Sweep the mains and the sub from the same spot, near where they cross over (each on its own, with the "
                 "mains' instance in this DAW), and pick both. Pink noise and music don't keep the phase this needs.";
        applyButton.setButtonText ("Apply");
        applyButton.setEnabled (false);
        repaint();
        return;
    }

    // Worked out again only when something it depends on changed.
    const auto settings = processor.getZoneSettings();
    const auto ownLatency = processor.getEngine().getSystemLatencyMs();
    juce::String inputs;
    inputs << juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (theirs.get())) << "|"
           << juce::String::toHexString (reinterpret_cast<juce::pointer_sized_int> (mine.get())) << "|" << mains->delayMs << "|"
           << static_cast<int> (mains->invert) << "|" << (mains->latencyMs ? juce::String (*mains->latencyMs) : "-") << "|"
           << settings.delayMs << "|" << static_cast<int> (settings.invert) << "|" << (ownLatency ? juce::String (*ownLatency) : "-");
    if (inputs != subInputs || ! subSuggestion)
    {
        subInputs = inputs;
        subSuggestion = suggestSubAlignment (*theirs, mains->latencyMs, mains->delayMs, mains->invert, *mine, ownLatency,
                                             settings.delayMs, settings.invert);
    }
    const auto& s = *subSuggestion;
    if (! s.ok)
    {
        result = juce::String::fromUTF8 (s.note.c_str());
        applyButton.setButtonText ("Apply");
        applyButton.setEnabled (false);
        repaint();
        return;
    }

    const auto hz = [] (double f) { return juce::String (juce::roundToInt (f)); };
    const auto shortOf = s.efficiencyDb > -0.5 ? juce::String ("close to a perfect sum")
                                               : db1 (-s.efficiencyDb) + " short of a perfect sum";
    result << "Crossover " << hz (s.regionLoHz) << "-" << hz (s.regionHiHz) << " Hz (crossing at " << hz (s.crossingHz) << " Hz).\n";
    if (! s.note.empty())
        result << juce::String::fromUTF8 (s.note.c_str()) << ".\n";
    else if (s.improvementDb < 0.05)
        result << "As set now: " << shortOf << " over the crossover.\n";
    else
        result << "Suggested: delay " << ms2 (s.delayMs) << ", polarity " << (s.invert ? "inverted" : "normal") << ".\n"
               << "Over the crossover: " << db1 (s.improvementDb) << " louder than now, " << shortOf << ".\n";
    result << "Confidence: " << roomeq::confidenceLabel (s.confidence);
    if (! s.reasons.empty())
        result << " (" << juce::String::fromUTF8 (s.reasons.front().c_str()) << ")";
    result << ".";

    const auto canApply = s.delayMs >= 0.0 && s.delayMs <= ZoneStage::maxDelayMs;
    const auto changes = std::abs (s.delayMs - settings.delayMs) > 0.005 || s.invert != settings.invert;
    applyButton.setButtonText ("Apply " + ms2 (s.delayMs) + (s.invert ? ", inverted" : ""));
    applyButton.setEnabled (canApply && changes);
    repaint();
}

void AlignmentPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::axis);
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (12.5f));
    g.drawFittedText (result, resultBounds, juce::Justification::topLeft, 5, 0.9f);
}

void AlignmentPanel::resized()
{
    auto r = getLocalBounds().withTrimmedTop (6);
    heading.setBounds (r.removeFromTop (22));
    r.removeFromTop (2);
    const auto row = [&r] (juce::Label& label, int height = 26)
    {
        auto line = r.removeFromTop (height);
        label.setBounds (line.removeFromLeft (108));
        r.removeFromTop (6);
        return line;
    };
    {
        auto line = row (latencyLabel);
        loopbackButton.setBounds (line.removeFromRight (86));
        line.removeFromRight (6);
        latencyValue.setBounds (line);
    }
    withBox.setBounds (row (withLabel));
    {
        const auto line = row (theirLabel);
        theirBox.setBounds (line);
        typedArrival.setBounds (line);
    }
    ownBox.setBounds (row (ownLabel));
    applyButton.setBounds (r.removeFromBottom (28));
    r.removeFromBottom (4);
    resultBounds = r;
}
