#include "plugin/ResponseGraph.h"

#include "plugin/Theme.h"

#include <cmath>

namespace
{
constexpr double fMin = 20.0, fMax = 20000.0;
constexpr double tickHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

juce::String tickLabel (double hz)
{
    return hz >= 1000.0 ? juce::String (juce::roundToInt (hz / 1000.0)) + "k" : juce::String (juce::roundToInt (hz));
}
} // namespace

void ResponseGraph::setData (std::shared_ptr<const MeasurementEngine::Display> newDisplay, int newSelectedId)
{
    display = std::move (newDisplay);
    selectedId = newSelectedId;
    if (display != nullptr && std::isfinite (display->summary.targetDb))
    {
        // 40 dB window, target a little above the middle, on 5 dB lines.
        dbTop = std::ceil ((display->summary.targetDb + 14.0) / 5.0) * 5.0;
        dbBottom = dbTop - 40.0;
    }
    repaint();
}

int ResponseGraph::selectedIndex() const
{
    if (display == nullptr)
        return -1;
    for (std::size_t i = 0; i < display->ids.size(); ++i)
        if (display->ids[i] == selectedId)
            return static_cast<int> (i);
    return -1;
}

juce::Rectangle<float> ResponseGraph::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (44.0f).withTrimmedRight (14.0f).withTrimmedTop (30.0f).withTrimmedBottom (24.0f);
}

float ResponseGraph::xFor (double hz, juce::Rectangle<float> area) const
{
    return area.getX() + area.getWidth() * static_cast<float> (std::log (hz / fMin) / std::log (fMax / fMin));
}

float ResponseGraph::yFor (double db, juce::Rectangle<float> area) const
{
    return area.getY() + area.getHeight() * static_cast<float> ((dbTop - db) / (dbTop - dbBottom));
}

juce::Path ResponseGraph::curve (const std::vector<double>& db, juce::Rectangle<float> area) const
{
    juce::Path p;
    auto drawing = false;
    const auto& grid = display->summary.grid;
    for (std::size_t i = 0; i < grid.size() && i < db.size(); ++i)
    {
        if (! std::isfinite (db[i]))
        {
            drawing = false;
            continue;
        }
        const juce::Point<float> pt { xFor (grid[i], area), yFor (db[i], area) };
        if (drawing)
            p.lineTo (pt);
        else
            p.startNewSubPath (pt);
        drawing = true;
    }
    return p;
}

void ResponseGraph::drawAxes (juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setFont (juce::FontOptions (11.0f));
    for (auto hz : tickHz)
    {
        const auto x = xFor (hz, area);
        g.setColour (theme::grid);
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
        g.setColour (theme::muted);
        g.drawText (tickLabel (hz), juce::Rectangle<float> (x - 20.0f, area.getBottom() + 4.0f, 40.0f, 16.0f),
                    juce::Justification::centred);
    }
    for (auto db = dbBottom; db <= dbTop + 0.1; db += 5.0)
    {
        const auto y = yFor (db, area);
        g.setColour (theme::grid);
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (theme::muted);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (0.0f, y - 8.0f, area.getX() - 6.0f, 16.0f),
                    juce::Justification::centredRight);
    }
    g.setColour (theme::axis);
    g.drawRect (area, 1.0f);
}

void ResponseGraph::drawLegend (juce::Graphics& g, juce::Rectangle<float> area) const
{
    struct Item { juce::String label; juce::Colour colour; bool dashed; };
    std::vector<Item> items { { "Average", theme::blue, false }, { "Each position", theme::muted, false },
                              { "Target (flat)", theme::ink, true } };
    if (selectedIndex() >= 0)
        items.push_back ({ "Selected", theme::orange, false });

    g.setFont (juce::FontOptions (12.0f));
    auto x = area.getX();
    const auto y = 8.0f;
    for (const auto& item : items)
    {
        juce::Path swatch;
        swatch.startNewSubPath (x, y + 7.0f);
        swatch.lineTo (x + 18.0f, y + 7.0f);
        g.setColour (item.colour);
        if (item.dashed)
        {
            juce::Path dashed;
            const float dashes[] = { 4.0f, 3.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, swatch, dashes, 2);
            g.fillPath (dashed);
        }
        else
        {
            g.strokePath (swatch, juce::PathStrokeType (2.0f));
        }
        g.setColour (theme::ink2);
        const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), item.label);
        g.drawText (item.label, juce::Rectangle<float> (x + 24.0f, y, w + 4.0f, 14.0f), juce::Justification::centredLeft);
        x += 24.0f + w + 20.0f;
    }
}

void ResponseGraph::drawHover (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (hoverX < area.getX() || hoverX > area.getRight())
        return;
    const auto& s = display->summary;
    const auto hz = fMin * std::pow (fMax / fMin, (hoverX - area.getX()) / area.getWidth());
    std::size_t i = 0;
    for (std::size_t k = 1; k < s.grid.size(); ++k)
        if (std::abs (std::log (s.grid[k] / hz)) < std::abs (std::log (s.grid[i] / hz)))
            i = k;

    g.setColour (theme::muted.withAlpha (0.6f));
    g.drawVerticalLine (juce::roundToInt (hoverX), area.getY(), area.getBottom());

    auto text = theme::formatHz (s.grid[i]);
    if (std::isfinite (s.averageDb[i]))
        text << "   average " << juce::String (s.averageDb[i], 1) << " dB";
    if (const auto sel = selectedIndex(); sel >= 0 && std::isfinite (s.positionDb[static_cast<std::size_t> (sel)][i]))
        text << "   selected " << juce::String (s.positionDb[static_cast<std::size_t> (sel)][i], 1) << " dB";
    text << "   target " << juce::String (s.targetDb, 1) << " dB";

    g.setFont (juce::FontOptions (12.0f));
    const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 16.0f;
    auto box = juce::Rectangle<float> (hoverX + 8.0f, area.getY() + 8.0f, w, 22.0f);
    if (box.getRight() > area.getRight())
        box.setX (hoverX - 8.0f - w);
    g.setColour (theme::plane.withAlpha (0.9f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (theme::ink);
    g.drawText (text, box, juce::Justification::centred);
}

void ResponseGraph::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    const auto area = plotArea();
    if (display == nullptr)
    {
        drawAxes (g, area);
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("No measurements yet. Connect the mic, then press Measure position.", area,
                    juce::Justification::centred);
        return;
    }

    const auto& s = display->summary;
    g.setColour (theme::plane.withAlpha (0.6f));   // outside the PA's usable range: nothing is corrected there
    g.fillRect (area.withRight (xFor (s.usable.first, area)));
    g.fillRect (area.withLeft (xFor (s.usable.second, area)));

    drawAxes (g, area);
    drawLegend (g, area);

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (area.toNearestInt());

    const auto selected = selectedIndex();
    for (std::size_t i = 0; i < s.positionDb.size(); ++i)
    {
        if (static_cast<int> (i) == selected)
            continue;
        const auto path = curve (s.positionDb[i], area);
        if (display->excluded[i])
        {
            juce::Path dashed;
            const float dashes[] = { 3.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, path, dashes, 2);
            g.setColour (theme::muted.withAlpha (0.35f));
            g.fillPath (dashed);
        }
        else
        {
            g.setColour (theme::muted.withAlpha (0.75f));
            g.strokePath (path, juce::PathStrokeType (1.2f));
        }
    }

    if (std::isfinite (s.targetDb))
    {
        juce::Path target, dashed;
        target.startNewSubPath (xFor (s.usable.first, area), yFor (s.targetDb, area));
        target.lineTo (xFor (s.usable.second, area), yFor (s.targetDb, area));
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, target, dashes, 2);
        g.setColour (theme::ink);
        g.fillPath (dashed);
    }

    if (selected >= 0)
    {
        g.setColour (theme::orange);
        g.strokePath (curve (s.positionDb[static_cast<std::size_t> (selected)], area), juce::PathStrokeType (2.0f));
    }

    g.setColour (theme::blue);
    g.strokePath (curve (s.averageDb, area), juce::PathStrokeType (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    drawHover (g, area);
}

void ResponseGraph::mouseMove (const juce::MouseEvent& e)
{
    hoverX = static_cast<float> (e.x);
    repaint();
}

void ResponseGraph::mouseExit (const juce::MouseEvent&)
{
    hoverX = -1.0f;
    repaint();
}
