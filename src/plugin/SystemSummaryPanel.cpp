#include "plugin/SystemSummaryPanel.h"

#include "plugin/Theme.h"

namespace
{
juce::String utf8 (const std::string& s)
{
    return juce::String::fromUTF8 (s.c_str());
}

juce::String signedDb (double db)
{
    return (db > 0.0 ? "+" : db < 0.0 ? juce::String::fromUTF8 ("\xe2\x88\x92") : juce::String()) + juce::String (std::abs (db), 1) + " dB";
}

juce::Colour ratingColour (roomeq::Rating r)
{
    return r == roomeq::Rating::excellent || r == roomeq::Rating::good ? theme::good
           : r == roomeq::Rating::fair                                 ? theme::warning
                                                                       : theme::critical;
}

juce::Colour confidenceColour (roomeq::Confidence c)
{
    return c == roomeq::Confidence::high ? theme::good : c == roomeq::Confidence::medium ? theme::warning : theme::critical;
}

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
} // namespace

SystemSummaryPanel::SystemSummaryPanel()
{
    why.setMultiLine (true, true);
    why.setReadOnly (true);
    why.setScrollbarsShown (true);
    why.setCaretVisible (false);
    why.setFont (juce::FontOptions (12.5f));
    why.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    why.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    why.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    why.setColour (juce::TextEditor::textColourId, theme::ink2);
    addAndMakeVisible (why);
}

void SystemSummaryPanel::setDisplay (std::shared_ptr<const MeasurementEngine::Display> display)
{
    if (display == shown)
        return;
    shown = std::move (display);
    summary = shown != nullptr ? shown->system : std::nullopt;

    juce::String text;
    if (summary)
    {
        for (const auto& e : summary->explanations)
            text << juce::String::fromUTF8 ("\xe2\x80\xa2 ") << utf8 (e.text) << "\n";
        if (summary->explanations.empty())
            text = "Nothing: the proposal follows the target wherever the measurements allow.";
    }
    why.setText (text.trimEnd(), false);
    why.setVisible (summary.has_value());
    repaint();
}

void SystemSummaryPanel::resized()
{
    // Two columns of five rows; the reasons beside them when the panel is wide, else below (scrolling).
    auto r = getLocalBounds().reduced (14, 8);
    const auto wide = r.getWidth() >= 1100;
    const auto rowsHeight = [&] (int available) { return juce::jlimit (17, 22, (available - 20) / 5); };
    if (wide)
    {
        auto left = r.removeFromLeft (juce::roundToInt (r.getWidth() * 0.56f));
        rowHeight = rowsHeight (left.getHeight());
        measuredArea = left.removeFromLeft (left.getWidth() / 2).withTrimmedRight (12);
        proposalArea = left.withTrimmedRight (12);
        whyHeading = r.removeFromTop (20);
        why.setBounds (r.withTrimmedLeft (-4));
    }
    else
    {
        rowHeight = rowsHeight (juce::jmin (r.getHeight() - 68, 20 + 5 * 22));
        auto top = r.removeFromTop (20 + 5 * rowHeight);
        measuredArea = top.removeFromLeft (top.getWidth() / 2).withTrimmedRight (12);
        proposalArea = top;
        r.removeFromTop (4);
        whyHeading = r.removeFromTop (18);
        why.setBounds (r.withTrimmedLeft (-4));
    }
}

void SystemSummaryPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    g.setColour (theme::axis);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 6.0f, 1.0f);
    auto r = getLocalBounds().reduced (14, 10);
    if (! summary)
    {
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Measure positions to see the system summary.", r, juce::Justification::centred);
        return;
    }
    const auto& s = *summary;
    auto measured = measuredArea;
    auto proposal = proposalArea;

    const auto heading = [&] (juce::Rectangle<int>& area, const juce::String& text)
    {
        g.setColour (theme::muted);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (text.toUpperCase(), area.removeFromTop (20), juce::Justification::centredLeft);
    };
    const auto row = [&] (juce::Rectangle<int>& area, const juce::String& name, const juce::String& value,
                          std::optional<std::pair<juce::String, juce::Colour>> word = {})
    {
        auto line = area.removeFromTop (rowHeight);
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (name, line.removeFromLeft (juce::jmin (170, line.getWidth() / 2)), juce::Justification::centredLeft);
        if (word)
        {
            chip (g, line.removeFromLeft (70).toFloat().reduced (0.0f, rowHeight > 19 ? 2.5f : 1.0f), word->first, word->second);
            line.removeFromLeft (8);
        }
        g.setColour (theme::ink);
        g.drawFittedText (value, line, juce::Justification::centredLeft, 1, 0.85f);
    };

    heading (measured, "Measurements");
    row (measured, "Confidence", s.confidenceReasons.empty() ? juce::String() : utf8 (s.confidenceReasons.front()),
         std::pair { juce::String (roomeq::confidenceLabel (s.confidence)).toUpperCase(), confidenceColour (s.confidence) });
    row (measured, "Positions", juce::String (s.positions) + " good");
    if (s.variationDb && s.coverage)
        row (measured, "Coverage consistency", juce::String::fromUTF8 ("\xc2\xb1") + juce::String (*s.variationDb, 1) + " dB",
             std::pair { juce::String (roomeq::ratingLabel (*s.coverage)), ratingColour (*s.coverage) });
    else
        row (measured, "Coverage consistency", "one position: nothing to compare");
    row (measured, "Usable bandwidth",
         utf8 (roomeq::formatHz (s.usable.first)) + juce::String::fromUTF8 (" \xe2\x80\x93 ") + utf8 (roomeq::formatHz (s.usable.second)));
    row (measured, "Largest broad issue",
         s.issueDb && s.issueHz ? signedDb (*s.issueDb) + " at about " + utf8 (roomeq::formatHz (*s.issueHz))
                                : juce::String::fromUTF8 ("\xe2\x80\x94"));

    heading (proposal, "Correction proposal");
    row (proposal, "Filters", juce::String (s.filters));
    row (proposal, "Largest cut", s.largestCutDb < -0.05 ? signedDb (s.largestCutDb) : juce::String ("none"));
    row (proposal, "Largest boost", s.largestBoostDb > 0.05 ? signedDb (s.largestBoostDb) : juce::String ("none"));
    row (proposal, "Nulls left alone", juce::String (s.nullsIgnored));
    if (std::isfinite (s.beforeDb) && std::isfinite (s.afterDb))
        row (proposal, "Expected improvement",
             juce::String (s.beforeDb, 1) + juce::String::fromUTF8 (" \xe2\x86\x92 ") + juce::String (s.afterDb, 1) + " dB from target (RMS)");

    g.setColour (theme::muted);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText ("WHERE IT HELD BACK", whyHeading, juce::Justification::centredLeft);
}
