#include "plugin/QualityCard.h"

#include "plugin/Theme.h"

namespace
{
juce::Colour ratingColour (roomeq::Rating r)
{
    switch (r)
    {
        case roomeq::Rating::excellent:
        case roomeq::Rating::good: return theme::good;
        case roomeq::Rating::fair: return theme::warning;
        case roomeq::Rating::poor: return theme::critical;
    }
    return theme::muted;
}

juce::Colour confidenceColour (roomeq::Confidence c)
{
    return c == roomeq::Confidence::high ? theme::good : c == roomeq::Confidence::medium ? theme::warning : theme::critical;
}

// A word in a rounded box, tinted by its colour (the word carries the meaning, not the colour).
void chip (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour colour)
{
    g.setColour (colour.withAlpha (0.22f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colour);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (text, r, juce::Justification::centred);
}

juce::String utf8 (const std::string& s)
{
    return juce::String::fromUTF8 (s.c_str());
}

juce::String kindOf (const roomeq::Capture& c)
{
    return c.kind == "program" ? "music" : c.kind == "noise" ? "pink noise" : "sweep";
}

juce::String usableText (const std::pair<double, double>& usable)
{
    if (! std::isfinite (usable.first) || ! std::isfinite (usable.second))
        return "not found: the reference band is too noisy";
    return theme::formatHz (usable.first) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + theme::formatHz (usable.second);
}

struct Row
{
    juce::String name;
    std::optional<roomeq::Rating> rating;
    juce::String detail;
};

// Short: the card is narrow. Each number is the worst graded band's; Details... has them all.
std::vector<Row> rowsFor (const roomeq::MeasurementQuality& q, const roomeq::Capture& c)
{
    const auto sweep = c.kind == "sweep";
    std::vector<Row> rows;
    rows.push_back ({ "Signal-to-noise", q.snr,
                      std::isfinite (q.snrDb) ? juce::String (juce::roundToInt (q.snrDb)) + " dB at " + utf8 (q.snrBand)
                                              : juce::String ("nothing heard") });
    rows.push_back ({ "Coherence", q.coherenceRating,
                      q.coherence ? juce::String (*q.coherence, 2) + " at " + utf8 (q.coherenceBand) : juce::String ("none for sweeps") });
    if (q.repeatDb)
        rows.push_back ({ "Repeatability", q.repeatability,
                          juce::String (std::max (*q.repeatDb, 0.0), 1) + " dB at " + utf8 (q.repeatBand) });
    else
        rows.push_back ({ "Repeatability", std::nullopt, sweep ? "one sweep" : "one pass" });
    rows.push_back ({ "Usable range", std::nullopt, usableText (q.usable) });
    return rows;
}
} // namespace

QualityCard::QualityCard()
{
    detailsButton.setTooltip ("The raw numbers: each octave band's level, SNR, coherence and repeat spread, the arrival, "
                              "and a sweep's impulse response.");
    detailsButton.onClick = [this]
    {
        if (auto details = createDetails())
            juce::CallOutBox::launchAsynchronously (std::move (details), detailsButton.getScreenBounds(), nullptr);
    };
    addChildComponent (detailsButton);
}

void QualityCard::setAdvanced (bool on)
{
    advanced = on;
    detailsButton.setVisible (advanced && quality.has_value());
    repaint();
}

void QualityCard::setEntry (std::optional<MeasurementEngine::Entry> newEntry, std::pair<double, double> referenceBand)
{
    const auto same = entry.has_value() == newEntry.has_value()
                      && (! entry || (entry->capture == newEntry->capture && entry->impulse == newEntry->impulse))
                      && band == referenceBand;
    if (same)
        return;
    entry = std::move (newEntry);
    band = referenceBand;
    quality.reset();
    if (entry && entry->capture != nullptr)
        quality = roomeq::measurementQuality (*entry->capture, band.first, band.second);
    detailsButton.setVisible (advanced && quality.has_value());
    repaint();
}

std::unique_ptr<juce::Component> QualityCard::createDetails() const
{
    if (! entry || ! quality)
        return nullptr;
    auto details = std::make_unique<QualityDetails> (*entry, *quality);
    details->setSize (640, entry->impulse != nullptr ? 470 : 330);
    return details;
}

void QualityCard::resized()
{
    detailsButton.setBounds (getWidth() - 86, 2, 86, 24);
}

void QualityCard::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (theme::axis);
    g.drawHorizontalLine (0, 0.0f, static_cast<float> (getWidth()));
    if (! entry || ! quality || entry->capture == nullptr)
    {
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText ("Select a capture to see its measurement quality.", r.withTrimmedTop (8), juce::Justification::topLeft, 2);
        return;
    }

    const auto& c = *entry->capture;
    const auto& q = *quality;
    r.removeFromTop (getHeight() >= 150 ? 6 : 2);
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("Measurement quality", r.removeFromTop (20).withTrimmedRight (92), juce::Justification::centredLeft);
    if (getHeight() >= 172)   // the subtitle, when there's room
    {
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (utf8 (c.name) + juce::String::fromUTF8 (" \xc2\xb7 ") + kindOf (c) + (entry->verify ? " (verify)" : "")
                        + (advanced ? juce::String::fromUTF8 (" \xc2\xb7 worst band shown") : juce::String()),
                    r.removeFromTop (16), juce::Justification::centredLeft);
    }
    r.removeFromTop (4);

    const auto labelWidth = 150, chipWidth = 76;
    // A row: its name, a rated word (or a dash when it doesn't apply) and the number behind it; or a plain value.
    const auto rowHeight = juce::jlimit (16, 22, (r.getHeight() - 4) / 5 - 2);   // five rows, tighter in a small window
    const auto line = [&] (const juce::String& name, std::optional<juce::String> word, juce::Colour colour, const juce::String& detail,
                           bool plain = false)
    {
        auto row = r.removeFromTop (rowHeight);
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (name, row.removeFromLeft (labelWidth), juce::Justification::centredLeft);
        r.removeFromTop (2);
        if (plain)
        {
            g.drawText (detail, row, juce::Justification::centredLeft);
            return;
        }
        auto chipArea = row.removeFromLeft (chipWidth).toFloat().reduced (0.0f, rowHeight > 20 ? 2.5f : 1.0f);
        if (word)
            chip (g, chipArea, *word, colour);
        else
        {
            g.setColour (theme::muted);
            g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94"), chipArea, juce::Justification::centred);
        }
        row.removeFromLeft (8);
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText (detail, row, juce::Justification::centredLeft, 1, 0.85f);
    };
    for (const auto& row : rowsFor (q, c))
    {
        const auto plain = row.name == "Usable range";
        // The number behind a rating is Advanced; what doesn't apply says why either way.
        const auto detail = plain || advanced || ! row.rating ? row.detail : juce::String();
        line (row.name, row.rating ? std::optional<juce::String> (roomeq::ratingLabel (*row.rating)) : std::nullopt,
              row.rating ? ratingColour (*row.rating) : theme::muted, detail, plain);
    }
    r.removeFromTop (4);
    line ("Measurement confidence", juce::String (roomeq::confidenceLabel (q.confidence)).toUpperCase(), confidenceColour (q.confidence), {});
    if (! q.reasons.empty())
    {
        juce::StringArray reasons;
        for (const auto& reason : q.reasons)
            reasons.add (utf8 (reason));
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (12.0f));
        const auto area = r.removeFromTop (juce::jmin (32, r.getHeight()));
        g.drawFittedText (reasons.joinIntoString ("; "), area, juce::Justification::topLeft, juce::jmax (1, area.getHeight() / 15), 0.85f);
    }
}

// ---------------------------------------------------------------------------

QualityDetails::QualityDetails (MeasurementEngine::Entry e, roomeq::MeasurementQuality q) : entry (std::move (e)), quality (std::move (q))
{
}

void QualityDetails::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    auto r = getLocalBounds().reduced (14, 12);
    const auto& c = *entry.capture;
    g.setColour (theme::ink);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (utf8 (c.name) + juce::String::fromUTF8 (" \xc2\xb7 ") + kindOf (c) + ": measurement details", r.removeFromTop (22),
                juce::Justification::centredLeft);
    r.removeFromTop (6);

    // Per octave band.
    const int widths[] = { 64, 76, 76, 84, 110, 110, 90 };
    const char* heads[] = { "Band", "Level", "SNR", "Coherence", "Repeat spread", "Beyond noise", "Grade" };
    const auto cells = [&] (juce::Rectangle<int> row, const juce::StringArray& text, juce::Colour colour)
    {
        g.setColour (colour);
        for (int i = 0; i < text.size(); ++i)
            g.drawText (text[i], row.removeFromLeft (widths[i]), i == 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
    };
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    {
        juce::StringArray h;
        for (auto* s : heads)
            h.add (s);
        cells (r.removeFromTop (18), h, theme::muted);
    }
    g.setFont (juce::FontOptions (12.5f));
    const auto dash = juce::String::fromUTF8 ("\xe2\x80\x94");
    for (const auto& b : c.grade.bands)
    {
        const auto grade = b.grade();
        juce::StringArray t;
        t.add (utf8 (b.name));
        t.add (juce::String (b.levelDb, 1) + " dB");
        t.add (juce::String (b.snrDb, 1) + " dB");
        t.add (b.coherence ? juce::String (*b.coherence, 3) : dash);
        t.add (b.spreadDb ? juce::String (*b.spreadDb, 2) + " dB" : dash);
        t.add (b.excessSpreadDb ? juce::String (*b.excessSpreadDb, 2) + " dB" : dash);
        t.add (grade ? juce::String (roomeq::gradeLabel (*grade)) : juce::String ("out of range"));
        cells (r.removeFromTop (17), t, b.outOfRange ? theme::muted : theme::ink2);
    }
    r.removeFromTop (8);

    // Arrival, drift, and why the confidence is what it is.
    juce::StringArray notes;
    if (std::isfinite (entry.arrival.ms))
    {
        auto a = "Arrival " + juce::String (entry.arrival.ms, 2) + " ms (" + roomeq::confidenceLabel (entry.arrival.confidence) + ")";
        for (const auto& reason : entry.arrival.reasons)
            a << "; " << utf8 (reason);
        notes.add (a);
    }
    if (std::isfinite (c.driftPpm) && std::abs (c.driftPpm) > 0.0)
        notes.add ("Clock drift " + juce::String (c.driftPpm, 1) + " ppm, corrected");
    auto conf = "Confidence " + juce::String (roomeq::confidenceLabel (quality.confidence));
    for (const auto& reason : quality.reasons)
        conf << "; " << utf8 (reason);
    notes.add (conf);
    g.setColour (theme::ink2);
    g.setFont (juce::FontOptions (12.5f));
    for (const auto& n : notes)
        g.drawFittedText (n, r.removeFromTop (18), juce::Justification::centredLeft, 1, 0.85f);

    if (entry.impulse != nullptr && ! entry.impulse->samples.empty())
    {
        r.removeFromTop (8);
        drawImpulse (g, r);
    }
}

void QualityDetails::drawImpulse (juce::Graphics& g, juce::Rectangle<int> area) const
{
    // Energy-time curve: |h| in dB below its peak, 0 to -60 dB.
    const auto& imp = *entry.impulse;
    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("Impulse response (energy, dB)", area.removeFromTop (16), juce::Justification::centredLeft);
    auto plot = area.withTrimmedLeft (34).withTrimmedBottom (16).toFloat();
    g.setColour (theme::plane);
    g.fillRect (plot);

    auto peak = 1e-30f;
    for (const auto v : imp.samples)
        peak = std::max (peak, std::abs (v));
    const auto n = imp.samples.size();
    const auto spanMs = 1000.0 * static_cast<double> (n) / imp.fs;
    const auto xOf = [&] (double ms) { return plot.getX() + static_cast<float> ((ms - imp.startMs) / spanMs) * plot.getWidth(); };
    const auto yOf = [&] (double db) { return plot.getY() + static_cast<float> (juce::jlimit (0.0, 1.0, -db / 60.0)) * plot.getHeight(); };

    g.setFont (juce::FontOptions (11.0f));
    for (int db = 0; db >= -60; db -= 20)
    {
        g.setColour (theme::grid);
        g.drawHorizontalLine (juce::roundToInt (yOf (db)), plot.getX(), plot.getRight());
        g.setColour (theme::muted);
        g.drawText (juce::String (db), juce::Rectangle<float> (plot.getX() - 34.0f, yOf (db) - 7.0f, 30.0f, 14.0f), juce::Justification::centredRight);
    }
    for (auto ms = std::ceil (imp.startMs / 10.0) * 10.0; ms <= imp.startMs + spanMs; ms += 10.0)
    {
        g.setColour (theme::grid);
        g.drawVerticalLine (juce::roundToInt (xOf (ms)), plot.getY(), plot.getBottom());
        g.setColour (theme::muted);
        g.drawText (juce::String (juce::roundToInt (ms)), juce::Rectangle<float> (xOf (ms) - 20.0f, plot.getBottom() + 1.0f, 40.0f, 14.0f),
                    juce::Justification::centred);
    }

    // One vertical line per pixel column: the loudest sample in it.
    juce::Path etc;
    const auto columns = juce::jmax (1, juce::roundToInt (plot.getWidth()));
    for (int x = 0; x < columns; ++x)
    {
        const auto from = static_cast<std::size_t> (static_cast<double> (x) / columns * static_cast<double> (n));
        const auto to = std::max (from + 1, static_cast<std::size_t> (static_cast<double> (x + 1) / columns * static_cast<double> (n)));
        auto loudest = 0.0f;
        for (auto i = from; i < to && i < n; ++i)
            loudest = std::max (loudest, std::abs (imp.samples[i]));
        const auto y = yOf (20.0 * std::log10 (std::max (loudest / peak, 1e-6f)));
        if (x == 0)
            etc.startNewSubPath (plot.getX(), y);
        else
            etc.lineTo (plot.getX() + static_cast<float> (x), y);
    }
    g.setColour (theme::blue);
    g.strokePath (etc, juce::PathStrokeType (1.2f));

    if (std::isfinite (entry.arrival.ms))
    {
        g.setColour (theme::orange);
        const auto x = xOf (entry.arrival.ms);
        g.drawLine (x, plot.getY(), x, plot.getBottom(), 1.0f);
        g.drawText ("arrival", juce::Rectangle<float> (x + 3.0f, plot.getY() + 2.0f, 60.0f, 14.0f), juce::Justification::centredLeft);
    }
    g.setColour (theme::muted);
    g.drawText ("ms", juce::Rectangle<float> (plot.getRight() - 20.0f, plot.getBottom() + 1.0f, 20.0f, 14.0f), juce::Justification::centredRight);
}
