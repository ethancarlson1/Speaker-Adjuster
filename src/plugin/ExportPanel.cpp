#include "plugin/ExportPanel.h"

#include "plugin/Theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{
template <typename T>
bool ready (const std::shared_future<T>& f)
{
    return f.valid() && f.wait_for (std::chrono::seconds (0)) == std::future_status::ready;
}

juce::String db1 (double v) { return juce::String (v, 1) + " dB"; }

constexpr int stripIdBus = 1, stripIdMatrix = 17, stripIdMainLR = 23, stripIdMainMC = 24;

int stripId (roomeq::x32::Destination d)
{
    switch (d.strip)
    {
        case roomeq::x32::Strip::bus: return stripIdBus + juce::jlimit (1, 16, d.number) - 1;
        case roomeq::x32::Strip::matrix: return stripIdMatrix + juce::jlimit (1, 6, d.number) - 1;
        case roomeq::x32::Strip::mainStereo: return stripIdMainLR;
        case roomeq::x32::Strip::mainMono: return stripIdMainMC;
    }
    return stripIdBus;
}

roomeq::x32::Destination destinationFor (int id)
{
    if (id >= stripIdMatrix && id < stripIdMainLR)
        return { roomeq::x32::Strip::matrix, id - stripIdMatrix + 1 };
    if (id == stripIdMainLR)
        return { roomeq::x32::Strip::mainStereo, 1 };
    if (id == stripIdMainMC)
        return { roomeq::x32::Strip::mainMono, 1 };
    return { roomeq::x32::Strip::bus, juce::jlimit (1, 16, id - stripIdBus + 1) };
}
} // namespace

ExportPanel::ExportPanel (roomeq::EqExport asPlayed, Options initial)
    : base (std::move (asPlayed)), options (initial)
{
    grid = roomeq::refitGrid();
    playedDb = roomeq::responseDb (base.correction, grid, base.sampleRate);

    formatBox.addItem ("Text, copied (for typing into a console)", 1);
    formatBox.addItem (juce::String::fromUTF8 ("CSV file\xe2\x80\xa6"), 2);
    formatBox.addItem (juce::String::fromUTF8 ("JSON file\xe2\x80\xa6"), 3);
    formatBox.addItem (juce::String::fromUTF8 ("Behringer X32 / Midas M32 snippet\xe2\x80\xa6"), 4);
    const auto count = correctionBands();
    bandsBox.addItem ("All " + juce::String (count) + (count == 1 ? " band" : " bands") + ", as played", 1);
    for (int i = 0; i < 3; ++i)
        bandsBox.addItem ("Fitted to " + juce::String (bandChoices[i]), i + 2);
    for (int i = 0; i < 3; ++i)
        bandsBox.setItemEnabled (i + 2, bandChoices[i] < count);
    for (int n = 1; n <= 16; ++n)
        stripBox.addItem ("Bus " + juce::String (n), stripIdBus + n - 1);
    for (int n = 1; n <= 6; ++n)
        stripBox.addItem ("Matrix " + juce::String (n), stripIdMatrix + n - 1);
    stripBox.addItem ("Main LR", stripIdMainLR);
    stripBox.addItem ("Main M/C", stripIdMainMC);

    formatBox.setTooltip ("Text to type into a console, a file another tool reads, or a snippet an X32 or M32 loads "
                          "(with X32-Edit / M32-Edit, or from a USB stick).");
    bandsBox.setTooltip ("For an output EQ with fewer bands than the correction uses: the correction fitted again with at "
                         "most this many, rather than the smallest dropped. The graph and the line under it show what it costs.");
    stripBox.setTooltip ("The X32 / M32 strip whose six-band EQ the snippet sets: a mix bus, a matrix, or a main.");
    for (auto* l : { &formatLabel, &bandsLabel, &stripLabel })
    {
        l->setColour (juce::Label::textColourId, theme::ink2);
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (*l);
    }
    for (auto* b : { &formatBox, &bandsBox, &stripBox })
    {
        b->onChange = [this, b] { readChoice (*b); };
        addAndMakeVisible (*b);
    }
    exportButton.onClick = [this]
    {
        if (const auto content = getContent(); content && onExport)
            onExport (options, *content, getSummary());
    };
    addAndMakeVisible (exportButton);
    setOptions (initial);
    setSize (preferredWidth, preferredHeight);
}

ExportPanel::~ExportPanel()
{
    stopTimer();
}

void ExportPanel::setOptions (const Options& o)
{
    options = o;
    if (options.bands != 0 && std::find (std::begin (bandChoices), std::end (bandChoices), options.bands) == std::end (bandChoices))
        options.bands = 0;
    const juce::ScopedValueSetter<bool> quiet (updating, true);
    formatBox.setSelectedId (static_cast<int> (options.format) + 1, juce::dontSendNotification);
    stripBox.setSelectedId (stripId (options.destination), juce::dontSendNotification);
    startFit();
    refresh();
}

void ExportPanel::readChoice (juce::ComboBox& box)
{
    if (updating)
        return;
    if (&box == &formatBox)
        options.format = static_cast<Format> (juce::jlimit (0, 3, formatBox.getSelectedId() - 1));
    else if (&box == &bandsBox)
    {
        const auto id = bandsBox.getSelectedId();
        options.bands = id >= 2 && id <= 4 ? bandChoices[id - 2] : 0;
    }
    else
        options.destination = destinationFor (stripBox.getSelectedId());
    startFit();
    refresh();
    if (onOptionsChanged)
        onOptionsChanged (options);
}

void ExportPanel::startFit()
{
    const auto bands = base.correction;
    const auto fs = base.sampleRate;
    if (options.format == Format::x32)
    {
        if (! desk.valid() && ! bands.empty())
            desk = std::async (std::launch::async, [bands, fs] { return roomeq::x32::fitForDesk (bands, fs); }).share();
    }
    else if (options.bands > 0 && options.bands < correctionBands() && refits.count (options.bands) == 0)
    {
        const auto n = options.bands;
        refits[n] = std::async (std::launch::async, [bands, fs, n] { return roomeq::refitBands (bands, fs, n); }).share();
    }
    if (isFitting())
        startTimerHz (20);
}

bool ExportPanel::isFitting() const
{
    if (options.format == Format::x32)
        return desk.valid() && ! ready (desk);
    const auto it = refits.find (options.bands);
    return options.bands < correctionBands() && it != refits.end() && ! ready (it->second);
}

void ExportPanel::waitForFit()
{
    if (options.format == Format::x32)
    {
        if (desk.valid())
            desk.wait();
    }
    else if (const auto it = refits.find (options.bands); it != refits.end())
        it->second.wait();
    refresh();
}

void ExportPanel::timerCallback()
{
    const auto pending = (desk.valid() && ! ready (desk))
                         || std::any_of (refits.begin(), refits.end(), [] (const auto& r) { return ! ready (r.second); });
    if (! pending)
        stopTimer();
    refresh();
}

std::optional<roomeq::Refit> ExportPanel::refitFor (int bands) const
{
    if (bands <= 0 || bands >= correctionBands())
        return std::nullopt;
    const auto it = refits.find (bands);
    if (it == refits.end() || ! ready (it->second))
        return std::nullopt;
    return it->second.get();
}

std::optional<roomeq::x32::Fit> ExportPanel::deskFit() const
{
    if (! ready (desk))
        return std::nullopt;
    return desk.get();
}

std::optional<roomeq::EqExport> ExportPanel::exportData() const
{
    if (options.bands <= 0 || options.bands >= correctionBands())
        return base;
    if (const auto r = refitFor (options.bands))
        return roomeq::withFewerBands (base, *r, options.bands);
    return std::nullopt;
}

std::optional<std::string> ExportPanel::getContent() const
{
    switch (options.format)
    {
        case Format::x32:
            if (const auto fit = deskFit())
                return roomeq::x32::snippet (*fit, options.destination, base.source);
            return std::nullopt;
        case Format::csv:
            if (const auto e = exportData())
                return roomeq::eqExportCsv (*e);
            return std::nullopt;
        case Format::json:
            if (const auto e = exportData())
                return roomeq::eqExportJson (*e);
            return std::nullopt;
        case Format::text:
            if (const auto e = exportData())
                return roomeq::eqExportText (*e);
            return std::nullopt;
    }
    return std::nullopt;
}

juce::String ExportPanel::getSummary() const
{
    if (options.format == Format::x32)
    {
        if (const auto fit = deskFit())
            return juce::String (fit->bands.size()) + " bells for " + roomeq::x32::destinationName (options.destination)
                   + ", within " + db1 (fit->maxErrorDb);
        return {};
    }
    if (const auto r = refitFor (options.bands))
        return "fitted to " + juce::String (options.bands) + " bands, within " + db1 (r->maxErrorDb);
    return {};
}

juce::String ExportPanel::getFitText() const
{
    const auto count = correctionBands();
    const auto dest = juce::String (roomeq::x32::destinationName (options.destination));
    if (count == 0)
        return options.format == Format::x32 ? "No correction to put on the desk yet."
                                             : "No correction yet: the export has the voicing, output gain, delay and polarity.";
    if (isFitting())
        return options.format == Format::x32 ? juce::String::fromUTF8 ("Fitting six bells on the desk's steps\xe2\x80\xa6")
                                             : juce::String::fromUTF8 ("Fitting the correction to ") + juce::String (options.bands)
                                                   + juce::String::fromUTF8 (" bands\xe2\x80\xa6");
    const auto within = [] (double maxDb, double rmsDb)
    { return "within " + db1 (maxDb) + " of the correction as it plays (" + db1 (rmsDb) + " RMS)."; };
    if (options.format == Format::x32)
    {
        if (const auto fit = deskFit())
            return juce::String (fit->bands.size()) + (fit->bands.size() == 1 ? " bell" : " bells") + " on " + dest
                   + "'s EQ, on the desk's steps: " + within (fit->maxErrorDb, fit->rmsErrorDb);
        return {};
    }
    if (const auto r = refitFor (options.bands))
        return "Fitted to " + juce::String (r->bands.size()) + " bands from " + juce::String (count) + ": "
               + within (r->maxErrorDb, r->rmsErrorDb);
    return "All " + juce::String (count) + (count == 1 ? " band" : " bands") + ", exactly as they play.";
}

void ExportPanel::refresh()
{
    const auto isX32 = options.format == Format::x32;
    {
        const juce::ScopedValueSetter<bool> quiet (updating, true);
        if (isX32)
            bandsBox.setText ("Six bells (the desk's six bands)", juce::dontSendNotification);
        else
            bandsBox.setSelectedId (options.bands > 0 && options.bands < correctionBands()
                                        ? 2 + static_cast<int> (std::find (std::begin (bandChoices), std::end (bandChoices), options.bands)
                                                                - std::begin (bandChoices))
                                        : 1,
                                    juce::dontSendNotification);
    }
    bandsBox.setEnabled (! isX32 && correctionBands() > bandChoices[2]);
    stripBox.setEnabled (isX32);
    stripLabel.setEnabled (isX32);

    exportDb.clear();
    if (isX32)
    {
        if (const auto fit = deskFit())
            exportDb = roomeq::responseDb (fit->bands, grid, base.sampleRate);
    }
    else if (const auto e = exportData())
        exportDb = e->fit ? roomeq::responseDb (e->correction, grid, base.sampleRate) : playedDb;

    exportButton.setButtonText (options.format == Format::text ? "Copy" : juce::String::fromUTF8 ("Save\xe2\x80\xa6"));
    exportButton.setEnabled (getContent().has_value() && ! (isX32 && correctionBands() == 0));
    repaint();
}

void ExportPanel::resized()
{
    auto r = getLocalBounds().reduced (16, 12);
    r.removeFromTop (28);   // the title
    const auto row = [&r] (juce::Label& label, juce::ComboBox& box)
    {
        auto line = r.removeFromTop (26);
        label.setBounds (line.removeFromLeft (140));
        box.setBounds (line);
        r.removeFromTop (6);
    };
    row (formatLabel, formatBox);
    row (bandsLabel, bandsBox);
    row (stripLabel, stripBox);
    r.removeFromTop (4);
    graphArea = r.removeFromTop (156);
    auto buttons = r.removeFromBottom (30);
    exportButton.setBounds (buttons.removeFromRight (110));
    r.removeFromBottom (6);
    textArea = r.withTrimmedTop (8);
}

void ExportPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Export the EQ", getLocalBounds().reduced (16, 12).removeFromTop (24), juce::Justification::centredLeft);

    // The correction as it plays and what the export plays, 20 Hz - 20 kHz.
    g.setColour (theme::plane);
    g.fillRect (graphArea);
    g.setColour (theme::axis);
    g.drawRect (graphArea);
    auto plot = graphArea.reduced (8, 18).toFloat();
    double largest = 3.0;
    for (const auto* curve : { &playedDb, &exportDb })
        for (auto v : *curve)
            largest = std::max (largest, std::abs (v));
    const auto range = 3.0 * std::ceil ((largest + 0.5) / 3.0);
    const auto xFor = [&] (double f) { return plot.getX() + plot.getWidth() * static_cast<float> (std::log10 (f / 20.0) / 3.0); };
    const auto yFor = [&] (double db) { return plot.getCentreY() - plot.getHeight() * 0.5f * static_cast<float> (db / range); };
    g.setFont (juce::FontOptions (11.0f));
    for (const auto f : { 100.0, 1000.0, 10000.0 })
    {
        g.setColour (theme::grid);
        g.drawVerticalLine (juce::roundToInt (xFor (f)), plot.getY(), plot.getBottom());
        g.setColour (theme::muted);
        g.drawText (theme::formatHz (f), juce::Rectangle<float> (xFor (f) + 3.0f, plot.getBottom() + 2.0f, 60.0f, 14.0f),
                    juce::Justification::centredLeft);
    }
    for (const auto db : { -range, -range / 2, range / 2, range })
    {
        g.setColour (theme::grid);
        g.drawHorizontalLine (juce::roundToInt (yFor (db)), plot.getX(), plot.getRight());
        g.setColour (theme::muted);
        // Below a line above 0 dB, above one below it: clear of the legend and the frequencies.
        g.drawText ((db > 0 ? "+" : "") + juce::String (juce::roundToInt (db)),
                    juce::Rectangle<float> (plot.getX() + 2.0f, db > 0 ? yFor (db) + 1.0f : yFor (db) - 13.0f, 40.0f, 12.0f),
                    juce::Justification::centredLeft);
    }
    g.setColour (theme::axis);
    g.drawHorizontalLine (juce::roundToInt (yFor (0.0)), plot.getX(), plot.getRight());
    const auto draw = [&] (const std::vector<double>& curve, juce::Colour colour, float thickness)
    {
        if (curve.size() != grid.size())
            return;
        juce::Path p;
        for (std::size_t i = 0; i < grid.size(); ++i)
        {
            const auto pt = juce::Point<float> (xFor (grid[i]), yFor (juce::jlimit (-range, range, curve[i])));
            i == 0 ? p.startNewSubPath (pt) : p.lineTo (pt);
        }
        g.setColour (colour);
        g.strokePath (p, juce::PathStrokeType (thickness));
    };
    draw (playedDb, theme::aqua, 2.2f);
    if (exportDb != playedDb)
        draw (exportDb, theme::gold, 1.6f);
    auto legend = graphArea.reduced (10, 3).removeFromTop (14);
    const auto key = [&] (const juce::String& text, juce::Colour colour)
    {
        g.setColour (colour);
        g.fillRect (legend.removeFromLeft (14).withSizeKeepingCentre (14, 3));
        legend.removeFromLeft (5);
        g.setColour (theme::ink2);
        g.drawText (text, legend.removeFromLeft (170), juce::Justification::centredLeft);
    };
    key ("Correction as it plays", theme::aqua);
    if (! exportDb.empty() && exportDb != playedDb)
        key (options.format == Format::x32 ? "On the desk" : "As exported", theme::gold);

    auto text = textArea;
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (getFitText(), text.removeFromTop (36), juce::Justification::topLeft, 2, 0.9f);
    text.removeFromTop (4);
    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (12.0f));
    const auto note = options.format == Format::x32
        ? juce::String ("Load it with X32-Edit / M32-Edit, or on the desk from a USB stick (Scenes, Snippets). It sets only "
                        "that strip's EQ: voicing, output gain, delay and polarity stay in the plugin. The desk's PEQ is taken "
                        "to shape a bell as the plugin does; to check, measure once through the desk with the plugin's "
                        "correction off.")
        : juce::String ("Bells and shelves as RBJ cookbook filters (the Q of a bell is its width between its half-gain points): "
                        "a console that measures Q another way needs other values. Includes the voicing bands that are on, the "
                        "output gain, delay and polarity; level compensation follows the show, so it isn't exported.");
    g.drawFittedText (note, text, juce::Justification::topLeft, 5, 0.9f);
}
