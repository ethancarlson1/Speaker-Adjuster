#include "plugin/ResponseGraph.h"

#include "plugin/PluginProcessor.h"
#include "plugin/Theme.h"

#include <array>
#include <cmath>
#include <functional>

namespace
{
constexpr double fMin = 20.0, fMax = 20000.0;
constexpr double tickHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
constexpr float handleRadius = 6.5f;

juce::String tickLabel (double hz)
{
    return hz >= 1000.0 ? juce::String (juce::roundToInt (hz / 1000.0)) + "k" : juce::String (juce::roundToInt (hz));
}

bool sameEq (const EqSettings& a, const EqSettings& b)
{
    return a.correctionOn == b.correctionOn && std::equal_to<double>() (a.amount, b.amount) && a.voicingOn == b.voicingOn
           && a.voicing == b.voicing && a.levelMatch == b.levelMatch;
}

void strokeDashed (juce::Graphics& g, const juce::Path& path, float width, float dash, float gap)
{
    juce::Path dashed;
    const float pattern[] = { dash, gap };
    juce::PathStrokeType (width).createDashedStroke (dashed, path, pattern, 2);
    g.fillPath (dashed);
}

double maxAbs (const std::vector<double>& v)
{
    double m = 0.0;
    for (auto x : v)
        if (std::isfinite (x))
            m = std::max (m, std::abs (x));
    return m;
}

std::size_t nearestIndex (const std::vector<double>& grid, double hz)
{
    std::size_t i = 0;
    for (std::size_t k = 1; k < grid.size(); ++k)
        if (std::abs (std::log (grid[k] / hz)) < std::abs (std::log (grid[i] / hz)))
            i = k;
    return i;
}
} // namespace

ResponseGraph::ResponseGraph (AdaptiveRoomEQProcessor& p)
    : processor (p), spectrogram (p.getMicFifo(), [&p] { return p.getSampleRate(); })
{
    using Panel = AdaptiveRoomEQProcessor::ShowPanel;
    const std::array<std::pair<juce::TextButton*, Panel>, 3> choices { { { &bothButton, Panel::both },
                                                                         { &spectrogramButton, Panel::spectrogram },
                                                                         { &eqOnlyButton, Panel::eq } } };
    for (const auto& [button, panel] : choices)
    {
        button->setRadioGroupId (7);
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, theme::blue.withAlpha (0.6f));
        button->onClick = [this, which = panel]
        {
            processor.setShowPanel (which);
            showPanelChanged();
        };
        addChildComponent (button);
    }
    bothButton.setTooltip ("The mic's spectrogram with the EQ curves over it: drag a voicing handle to where you see a build-up.");
    spectrogramButton.setTooltip ("Just the mic's spectrogram (the last 20 s, newest at the top).");
    eqOnlyButton.setTooltip ("Just the EQ curves and voicing handles.");
    updateCurves();
}

void ResponseGraph::resized()
{
    if (showMode)
    {
        const auto area = eqArea().toNearestInt();
        spectrogram.setSize (area.getWidth(), area.getHeight());
        auto row = juce::Rectangle<int> (area.getRight() - 214, area.getY() - 25, 214, 21);
        eqOnlyButton.setBounds (row.removeFromRight (44));
        spectrogramButton.setBounds (row.removeFromRight (100));
        bothButton.setBounds (row.removeFromRight (60));
    }
}

void ResponseGraph::showPanelChanged()
{
    using Panel = AdaptiveRoomEQProcessor::ShowPanel;
    const auto panel = processor.getShowPanel();
    bothButton.setToggleState (panel == Panel::both, juce::dontSendNotification);
    spectrogramButton.setToggleState (panel == Panel::spectrogram, juce::dontSendNotification);
    eqOnlyButton.setToggleState (panel == Panel::eq, juce::dontSendNotification);
    repaint();
}

bool ResponseGraph::showsSpectrogram() const
{
    return showMode && processor.getShowPanel() != AdaptiveRoomEQProcessor::ShowPanel::eq;
}

bool ResponseGraph::showsEq() const
{
    return ! showMode || processor.getShowPanel() != AdaptiveRoomEQProcessor::ShowPanel::spectrogram;
}

void ResponseGraph::timerCallback()
{
    // The spectrogram keeps up while it's on screen (a session can open in the
    // show view before the editor has a window, so the timer runs regardless).
    if (! showsSpectrogram() || ! isShowing())
        return;
    spectrogram.update();
    repaint (eqArea().expanded (2.0f).toNearestInt());
}

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
    updateCurves();
    repaint();
}

void ResponseGraph::refresh()
{
    const auto eq = processor.getEqSettings();
    const auto& engine = processor.getEngine();
    if (! sameEq (eq, lastEq) || engine.getPlaying() != lastApplied || display != lastDisplay
        || processor.getTarget() != lastTarget || engine.isComparingPrevious() != lastComparing
        || loudnessBands (processor.getLoudnessStatus()) != lastLoudness
        || processor.getLoudnessSettings().on != lastLoudnessOn
        || processor.getLoudness().getInfo().calibrated != lastLoudnessCalibrated
        || std::abs (processor.getMakeupDb() - curves.makeupDb) > 0.005f)
    {
        updateCurves();
        repaint();
    }
    if (showMode)
    {
        const auto info = processor.getShow().getInfo();
        const auto blocks = info.hasReference ? info.blocksHeard + 1000 * info.blocksDropped : -2;
        if (blocks != lastShowBlocks)
        {
            lastShowBlocks = blocks;
            repaint();
        }
        else if (const auto countdown = showCountdown (info); countdown != lastCountdown)
        {
            lastCountdown = countdown;
            repaint (getLocalBounds().removeFromTop (30));
        }
    }
}

void ResponseGraph::setShowMode (bool shouldShow)
{
    if (showMode != shouldShow)
    {
        showMode = shouldShow;
        for (auto* b : { &bothButton, &spectrogramButton, &eqOnlyButton })
            b->setVisible (showMode);
        if (showMode)
            startTimerHz (30);
        else
            stopTimer();
        showPanelChanged();
        resized();
    }
}

void ResponseGraph::setSelectedVoicingBand (int band)
{
    selectedBand = juce::jlimit (0, roomeq::numVoicingBands - 1, band);
    repaint();
}

void ResponseGraph::updateCurves()
{
    const auto& engine = processor.getEngine();
    lastEq = processor.getEqSettings();
    lastApplied = engine.getPlaying();
    lastDisplay = display;
    lastTarget = processor.getTarget();
    lastComparing = engine.isComparingPrevious();

    curves.grid = display != nullptr ? display->summary.grid : roomeq::logFreqGrid (20.0, 20000.0, 48);
    const auto fs = display != nullptr ? display->fitFs : 48000.0;
    const auto scaledBy = [] (const std::vector<roomeq::Band>& bands, double amount)
    {
        std::vector<roomeq::Band> out;
        for (const auto& b : bands)
            out.push_back (b.scaled (amount));
        return out;
    };
    curves.correctionOn = lastEq.correctionOn;
    curves.voicingOn = lastEq.voicingOn;
    curves.levelMatch = lastEq.levelMatch;
    curves.makeupDb = processor.getMakeupDb();
    curves.applied = roomeq::responseDb (scaledBy (lastApplied, lastEq.amount), curves.grid, fs);
    const auto* proposal = display != nullptr && display->proposal ? &*display->proposal : nullptr;
    curves.showProposal = proposal != nullptr && proposal->bands != engine.getApplied();
    curves.proposal = proposal != nullptr ? roomeq::responseDb (scaledBy (proposal->bands, lastEq.amount), curves.grid, fs)
                                          : std::vector<double> (curves.grid.size(), 0.0);
    curves.voicing = roomeq::responseDb (processor.getVoicingSections(), curves.grid, fs);

    // Loudness: what the stage applies at the current level (plugin only).
    lastLoudness = loudnessBands (processor.getLoudnessStatus());
    lastLoudnessOn = processor.getLoudnessSettings().on;
    lastLoudnessCalibrated = processor.getLoudness().getInfo().calibrated;
    curves.showLoudness = ! processor.isStandalone();
    curves.loudnessOn = lastLoudnessOn;
    curves.loudnessCalibrated = lastLoudnessCalibrated;
    curves.loudness = roomeq::responseDb (lastLoudness, curves.grid, fs);

    predicted.clear();
    targetDb.clear();
    if (display != nullptr)
    {
        targetOffset = roomeq::anchorOffsetDb (curves.grid, display->summary.averageDb, lastTarget);
        targetDb = lastTarget.db (curves.grid);
        for (auto& v : targetDb)
            v += targetOffset;

        const auto& avg = display->summary.averageDb;
        const auto& corr = curves.showProposal ? curves.proposal : curves.applied;
        predicted.resize (avg.size());
        for (std::size_t i = 0; i < avg.size(); ++i)
            predicted[i] = avg[i] + (curves.correctionOn ? corr[i] : 0.0) + (curves.voicingOn ? curves.voicing[i] : 0.0);
    }
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

const std::vector<double>* ResponseGraph::selectedCurve() const
{
    if (display == nullptr || selectedId < 0)
        return nullptr;
    if (const auto i = selectedIndex(); i >= 0)
        return &display->summary.positionDb[static_cast<std::size_t> (i)];
    for (std::size_t i = 0; i < display->verifyIds.size(); ++i)
        if (display->verifyIds[i] == selectedId)
            return &display->verifyDb[i];
    return nullptr;
}

juce::String ResponseGraph::selectedName() const
{
    for (const auto& e : processor.getEngine().getEntries())
        if (e.id == selectedId)
            return juce::String::fromUTF8 (e.capture->name.c_str());
    return {};
}

bool ResponseGraph::customTargetEditable() const
{
    return ! showMode && display != nullptr && processor.getTargetChoice() == AdaptiveRoomEQProcessor::targetCustom
           && std::isfinite (targetOffset);
}

// ---------------------------------------------------------------------------
// Geometry

// Setup: response (66%) and EQ strip. Show: change since soundcheck (40%), and
// the spectrogram/EQ panel. Each has a legend row above it.
juce::Rectangle<float> ResponseGraph::responseArea() const
{
    const auto full = getLocalBounds().toFloat().withTrimmedLeft (44.0f).withTrimmedRight (14.0f);
    const auto plotHeight = full.getHeight() - 30.0f - 30.0f - 22.0f;
    return full.withTrimmedTop (30.0f).withHeight (plotHeight * (showMode ? 0.40f : 0.66f));
}

juce::Rectangle<float> ResponseGraph::eqArea() const
{
    const auto r = responseArea();
    const auto bottom = static_cast<float> (getHeight()) - 22.0f;
    return r.withY (r.getBottom() + 30.0f).withBottom (bottom);
}

void ResponseGraph::drawShowPanel (juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto spectrum = showsSpectrogram(), eq = showsEq();
    const auto reserve = 222.0f;   // the Both / Spectrogram / EQ buttons
    if (spectrum)
        spectrogram.draw (g, area.toNearestInt(), eq ? 0.7f : 1.0f);
    if (eq)
    {
        drawEq (g, area, spectrum, reserve);
    }
    else
    {
        drawFrequencyAxis (g, area, true);
        g.setColour (theme::axis);
        g.drawRect (area, 1.0f);
        // Legend row: what it is, and the colour scale (when there's room).
        const auto rowY = area.getY() - 22.0f;
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (theme::ink2);
        const auto title = "Mic spectrogram, last " + juce::String (juce::roundToInt (Spectrogram::historySeconds)) + " s";
        const auto titleWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), title) + 8.0f;
        g.drawText (title, juce::Rectangle<float> (area.getX(), rowY, titleWidth, 18.0f), juce::Justification::centredLeft);
        auto key = juce::Rectangle<float> (area.getRight() - reserve - 250.0f, rowY, 250.0f, 18.0f);
        if (key.getX() > area.getX() + titleWidth + 12.0f)
        {
            g.setColour (theme::muted);
            g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92") + juce::String (juce::roundToInt (Spectrogram::rangeDb)) + " dB",
                        key.removeFromLeft (52.0f), juce::Justification::centredRight);
            g.drawText ("loudest", key.removeFromRight (58.0f), juce::Justification::centredLeft);
            const auto bar = key.reduced (6.0f, 5.0f);
            for (int i = 0; i < static_cast<int> (bar.getWidth()); ++i)
            {
                g.setColour (Spectrogram::colourFor (static_cast<float> (i) / bar.getWidth()));
                g.fillRect (juce::Rectangle<float> (bar.getX() + static_cast<float> (i), bar.getY(), 1.0f, bar.getHeight()));
            }
        }
        // Time: the newest at the top.
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (theme::muted);
        g.drawText ("now", juce::Rectangle<float> (0.0f, area.getY() - 2.0f, area.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
        g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92") + juce::String (juce::roundToInt (Spectrogram::historySeconds)) + " s",
                    juce::Rectangle<float> (0.0f, area.getBottom() - 12.0f, area.getX() - 6.0f, 14.0f), juce::Justification::centredRight);
    }
    if (spectrum && ! spectrogram.hearsTheMic())
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Spectrogram: waiting for the mic", area.withTrimmedTop (area.getHeight() * 0.3f).withHeight (20.0f),
                    juce::Justification::centred);
    }
}

juce::Point<float> ResponseGraph::getShowBandPosition (int band) const
{
    const auto area = responseArea();
    return { xFor (roomeq::recheckBands()[static_cast<std::size_t> (band)], area), area.getCentreY() };
}

juce::Point<float> ResponseGraph::getShowPanelPoint (double hz) const
{
    const auto area = eqArea();
    return { xFor (hz, area), area.getCentreY() };
}

int ResponseGraph::showBandAt (juce::Point<float> p) const
{
    const auto area = responseArea();
    if (! showMode || ! area.contains (p))
        return -1;
    const auto hz = hzFor (p.x, area);
    const auto& bands = roomeq::recheckBands();
    for (std::size_t b = 0; b < bands.size(); ++b)
        if (hz >= bands[b] * std::exp2 (-1.0 / 6.0) && hz < bands[b] * std::exp2 (1.0 / 6.0))
            return static_cast<int> (b);
    return -1;
}

juce::String ResponseGraph::showHoverText (juce::Point<float> p) const
{
    const auto dash = juce::String::fromUTF8 ("\xe2\x80\x93");
    if (const auto b = showBandAt (p); b >= 0)
    {
        const auto info = processor.getShow().getInfo();
        const auto centre = roomeq::recheckBands()[static_cast<std::size_t> (b)];
        const auto name = juce::String (roomeq::showBandNames()[static_cast<std::size_t> (b)]);
        const auto lo = theme::formatHz (centre * std::exp2 (-1.0 / 6.0)), hi = theme::formatHz (centre * std::exp2 (1.0 / 6.0));
        const auto sameUnit = lo.endsWith (" kHz") == hi.endsWith (" kHz");   // "178-225 Hz", "891 Hz-1.1 kHz"
        juce::String text = (name.endsWith ("k") ? name.dropLastCharacters (1) + " kHz" : name + " Hz") + " band ("
                            + (sameUnit ? lo.upToFirstOccurrenceOf (" ", false, false) : lo) + dash + hi + "): ";
        const auto& st = info.state;
        const auto i = static_cast<std::size_t> (b);
        if (! info.hasReference)
            return text + "no soundcheck reference yet";
        if (i >= st.deltaDb.size() || ! std::isfinite (st.deltaDb[i]))
            return text + "not heard clearly enough yet";
        const auto d = st.deltaDb[i];
        text << (d >= 0.0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) << juce::String (std::abs (d), 1) << " dB since soundcheck";
        if (i < st.flags.size() && st.flags[i] != 0)
            text << (std::abs (d) >= 6.0 ? " (flagged, 6 dB or more)" : " (flagged)");
        return text;
    }
    const auto panel = eqArea();
    if (showMode && panel.contains (p) && ! curves.grid.empty())
    {
        const auto i = nearestIndex (curves.grid, hzFor (p.x, panel));
        auto text = theme::formatHz (curves.grid[i]);
        if (showsEq())
        {
            text << "   |   correction " << juce::String (curves.applied[i], 1) << " dB, voicing " << juce::String (curves.voicing[i], 1) << " dB";
            if (curves.showLoudness && curves.loudnessOn && curves.loudnessCalibrated)
                text << ", loudness " << juce::String (curves.loudness[i], 1) << " dB";
        }
        return text;
    }
    return {};
}

void ResponseGraph::drawShowHover (juce::Graphics& g) const
{
    if (hoverX < 0.0f || draggingBand >= 0)
        return;
    const juce::Point<float> p (hoverX, hoverY);
    const auto text = showHoverText (p);
    if (text.isEmpty())
        return;
    const auto inChange = showBandAt (p) >= 0;
    const auto area = inChange ? responseArea() : eqArea();
    if (! inChange)
    {
        g.setColour (theme::ink.withAlpha (0.5f));
        g.drawVerticalLine (juce::roundToInt (hoverX), area.getY(), area.getBottom());
    }
    g.setFont (juce::FontOptions (12.0f));
    const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 16.0f;
    auto box = juce::Rectangle<float> (hoverX + 10.0f, juce::jlimit (area.getY() + 4.0f, area.getBottom() - 26.0f, hoverY - 30.0f), w, 22.0f);
    if (box.getRight() > area.getRight())
        box.setX (juce::jmax (area.getX(), hoverX - 10.0f - w));
    g.setColour (theme::plane.withAlpha (0.92f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (theme::axis);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);
    g.setColour (theme::ink);
    g.drawText (text, box, juce::Justification::centred);
}

float ResponseGraph::xFor (double hz, juce::Rectangle<float> area) const
{
    return area.getX() + area.getWidth() * static_cast<float> (std::log (hz / fMin) / std::log (fMax / fMin));
}

double ResponseGraph::hzFor (float x, juce::Rectangle<float> area) const
{
    return juce::jlimit (fMin, fMax, fMin * std::pow (fMax / fMin, static_cast<double> ((x - area.getX()) / area.getWidth())));
}

float ResponseGraph::yFor (double db, juce::Rectangle<float> area) const
{
    return area.getY() + area.getHeight() * static_cast<float> ((dbTop - db) / (dbTop - dbBottom));
}

double ResponseGraph::dbFor (float y, juce::Rectangle<float> area) const
{
    return dbTop - static_cast<double> ((y - area.getY()) / area.getHeight()) * (dbTop - dbBottom);
}

float ResponseGraph::eqYFor (double db, juce::Rectangle<float> area) const
{
    return area.getY() + area.getHeight() * static_cast<float> ((eqTop - db) / (eqTop - eqBottom));
}

double ResponseGraph::eqDbFor (float y, juce::Rectangle<float> area) const
{
    return eqTop - static_cast<double> ((y - area.getY()) / area.getHeight()) * (eqTop - eqBottom);
}

juce::Path ResponseGraph::curve (const std::vector<double>& grid, const std::vector<double>& db, juce::Rectangle<float> area,
                                 bool eq) const
{
    juce::Path p;
    auto drawing = false;
    for (std::size_t i = 0; i < grid.size() && i < db.size(); ++i)
    {
        if (! std::isfinite (db[i]))
        {
            drawing = false;
            continue;
        }
        const juce::Point<float> pt { xFor (grid[i], area), eq ? eqYFor (db[i], area) : yFor (db[i], area) };
        if (drawing)
            p.lineTo (pt);
        else
            p.startNewSubPath (pt);
        drawing = true;
    }
    return p;
}

juce::Point<float> ResponseGraph::voicingHandle (int band, juce::Rectangle<float> area) const
{
    const auto& v = lastEq.voicing[static_cast<std::size_t> (band)];
    const auto gain = roomeq::hasGain (v.type) ? v.gainDb : 0.0;
    return { xFor (juce::jlimit (fMin, fMax, v.freq), area),
             juce::jlimit (area.getY(), area.getBottom(), eqYFor (gain, area)) };
}

int ResponseGraph::voicingHandleAt (juce::Point<float> p) const
{
    if (! showsEq())
        return -1;
    const auto area = eqArea();
    auto best = -1;
    auto bestDistance = handleRadius + 4.0f;
    for (int b = 0; b < roomeq::numVoicingBands; ++b)
    {
        const auto d = voicingHandle (b, area).getDistanceFrom (p);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = b;
        }
    }
    return best;
}

juce::Point<float> ResponseGraph::targetPoint (std::size_t index, juce::Rectangle<float> area) const
{
    const auto& pt = lastTarget.points[index];
    return { xFor (pt.first, area), yFor (targetOffset + pt.second, area) };
}

int ResponseGraph::targetPointAt (juce::Point<float> p) const
{
    if (! customTargetEditable())
        return -1;
    const auto area = responseArea();
    for (std::size_t i = 0; i < lastTarget.points.size(); ++i)
        if (targetPoint (i, area).getDistanceFrom (p) < 9.0f)
            return static_cast<int> (i);
    return -1;
}

// ---------------------------------------------------------------------------
// Drawing

void ResponseGraph::drawFrequencyAxis (juce::Graphics& g, juce::Rectangle<float> area, bool labels) const
{
    g.setFont (juce::FontOptions (11.0f));
    for (auto hz : tickHz)
    {
        const auto x = xFor (hz, area);
        g.setColour (theme::grid);
        g.drawVerticalLine (juce::roundToInt (x), area.getY(), area.getBottom());
        if (labels)
        {
            g.setColour (theme::muted);
            g.drawText (tickLabel (hz), juce::Rectangle<float> (x - 20.0f, area.getBottom() + 4.0f, 40.0f, 16.0f),
                        juce::Justification::centred);
        }
    }
}

void ResponseGraph::drawLevelAxis (juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setFont (juce::FontOptions (11.0f));
    for (auto db = dbBottom; db <= dbTop + 0.1; db += 5.0)
    {
        const auto y = yFor (db, area);
        g.setColour (theme::grid);
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (theme::muted);
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (0.0f, y - 8.0f, area.getX() - 6.0f, 16.0f),
                    juce::Justification::centredRight);
    }
}

void ResponseGraph::drawEqAxis (juce::Graphics& g, juce::Rectangle<float> area) const
{
    g.setFont (juce::FontOptions (11.0f));
    for (int db = -12; db <= 12; db += 6)
    {
        const auto y = eqYFor (db, area);
        g.setColour (db == 0 ? theme::axis : theme::grid);
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (theme::muted);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                    juce::Rectangle<float> (0.0f, y - 8.0f, area.getX() - 6.0f, 16.0f), juce::Justification::centredRight);
    }
}

float ResponseGraph::drawLegend (juce::Graphics& g, juce::Point<float> at, const std::vector<LegendItem>& items)
{
    g.setFont (juce::FontOptions (12.0f));
    auto x = at.x;
    for (const auto& item : items)
    {
        juce::Path swatch;
        swatch.startNewSubPath (x, at.y + 7.0f);
        swatch.lineTo (x + 18.0f, at.y + 7.0f);
        g.setColour (item.colour);
        if (item.dashed)
            strokeDashed (g, swatch, 1.6f, 4.0f, 3.0f);
        else
            g.strokePath (swatch, juce::PathStrokeType (2.2f));
        g.setColour (theme::ink2);
        const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), item.label);
        g.drawText (item.label, juce::Rectangle<float> (x + 24.0f, at.y, w + 4.0f, 14.0f), juce::Justification::centredLeft);
        x += 24.0f + w + 18.0f;
    }
    return x - 18.0f;
}

void ResponseGraph::drawResponse (juce::Graphics& g, juce::Rectangle<float> area) const
{
    const auto& s = display->summary;
    const auto range = display->proposal ? display->proposal->fitRange : s.usable;
    g.setColour (theme::plane.withAlpha (0.6f));   // outside the corrected range
    g.fillRect (area.withRight (xFor (juce::jlimit (fMin, fMax, range.first), area)));
    g.fillRect (area.withLeft (xFor (juce::jlimit (fMin, fMax, range.second), area)));
    drawFrequencyAxis (g, area, false);
    drawLevelAxis (g, area);
    g.setColour (theme::axis);
    g.drawRect (area, 1.0f);

    const auto selected = selectedIndex();
    const auto showPredicted = ! predicted.empty()
                               && (maxAbs (curves.voicing) > 0.05 || maxAbs (curves.applied) > 0.05 || curves.showProposal);
    std::vector<LegendItem> legend { { "Average", theme::blue, false }, { "Each position", theme::muted, false },
                                     { "Target (" + juce::String (lastTarget.name).toLowerCase() + ")", theme::ink, true } };
    if (showPredicted)
        legend.push_back ({ curves.showProposal ? "Predicted with proposal" : "Predicted", theme::aqua, false });
    if (! display->verifiedDb.empty())
        legend.push_back ({ "Verified (" + juce::String (display->verifiedCount) + ")", theme::violet, false });
    const auto* chosen = selectedCurve();
    if (chosen != nullptr)
        legend.push_back ({ "Selected: " + selectedName(), theme::orange, false });
    drawLegend (g, { area.getX(), 8.0f }, legend);

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (area.toNearestInt());

    for (std::size_t i = 0; i < s.positionDb.size(); ++i)
    {
        if (static_cast<int> (i) == selected)
            continue;
        const auto path = curve (s.grid, s.positionDb[i], area, false);
        if (display->excluded[i])
        {
            g.setColour (theme::muted.withAlpha (0.35f));
            strokeDashed (g, path, 1.0f, 3.0f, 4.0f);
        }
        else
        {
            g.setColour (theme::muted.withAlpha (chosen != nullptr ? 0.3f : 0.6f));   // stand back while one is selected
            g.strokePath (path, juce::PathStrokeType (1.1f));
        }
    }

    if (! targetDb.empty())
    {
        const auto path = curve (s.grid, targetDb, area, false);
        g.setColour (theme::ink.withAlpha (0.35f));
        strokeDashed (g, path, 1.4f, 6.0f, 4.0f);
        juce::Graphics::ScopedSaveState inRange (g);
        g.reduceClipRegion (area.withLeft (xFor (juce::jlimit (fMin, fMax, range.first), area))
                                .withRight (xFor (juce::jlimit (fMin, fMax, range.second), area)).toNearestInt());
        g.setColour (theme::ink);
        strokeDashed (g, path, 1.5f, 6.0f, 4.0f);
    }

    g.setColour (theme::blue);
    g.strokePath (curve (s.grid, s.averageDb, area, false),
                  juce::PathStrokeType (2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (showPredicted)
    {
        g.setColour (theme::aqua);
        g.strokePath (curve (s.grid, predicted, area, false), juce::PathStrokeType (2.0f));
    }
    if (! display->verifiedDb.empty())
    {
        g.setColour (theme::violet);
        g.strokePath (curve (s.grid, display->verifiedDb, area, false), juce::PathStrokeType (2.0f));
    }

    // The selected capture on top of everything, outlined so it reads over the other curves.
    if (chosen != nullptr)
    {
        const auto path = curve (s.grid, *chosen, area, false);
        g.setColour (theme::plane.withAlpha (0.85f));
        g.strokePath (path, juce::PathStrokeType (5.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (theme::orange);
        g.strokePath (path, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (customTargetEditable())
        for (std::size_t i = 0; i < lastTarget.points.size(); ++i)
        {
            const auto p = targetPoint (i, area);
            g.setColour (theme::surface);
            g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (p));
            g.setColour (i == static_cast<std::size_t> (draggingPoint) ? theme::orange : theme::ink);
            g.drawEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre (p), 1.8f);
        }
}

void ResponseGraph::drawEq (juce::Graphics& g, juce::Rectangle<float> area, bool overImage, float reserveRight) const
{
    drawFrequencyAxis (g, area, true);
    drawEqAxis (g, area);
    g.setColour (theme::axis);
    g.drawRect (area, 1.0f);

    std::vector<LegendItem> legend { { curves.correctionOn ? "Correction" : "Correction (off)", theme::aqua, ! curves.correctionOn } };
    if (curves.showProposal)
        legend.push_back ({ "Proposed (not applied)", theme::aqua.withAlpha (0.7f), true });
    legend.push_back ({ curves.voicingOn ? "Voicing EQ" : "Voicing EQ (off)", theme::magenta, ! curves.voicingOn });
    const auto loudnessActive = curves.showLoudness && curves.loudnessOn && curves.loudnessCalibrated;
    if (curves.showLoudness)
        legend.push_back ({ ! curves.loudnessOn ? "Loudness (off)" : curves.loudnessCalibrated ? "Loudness now" : "Loudness (not calibrated)",
                            theme::gold, ! loudnessActive });
    const auto legendEnd = drawLegend (g, { area.getX(), area.getY() - 22.0f }, legend);

    // Output level match, on the legend row's right (left out when there's no room).
    {
        const auto r = std::round (curves.makeupDb * 10.0f) / 10.0f;
        const auto amount = std::abs (r) < 0.05f ? juce::String ("0.0")
                                                 : (r > 0.0f ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (r), 1);
        g.setColour (curves.levelMatch ? theme::ink2 : theme::muted);
        g.setFont (juce::FontOptions (12.0f));
        const auto text = curves.levelMatch ? "Output level matched: " + amount + " dB" : juce::String ("Output level match off");
        const auto box = juce::Rectangle<float> (area.getRight() - reserveRight - 240.0f, area.getY() - 24.0f, 236.0f, 20.0f);
        if (box.getRight() - juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) > legendEnd + 12.0f)
            g.drawText (text, box, juce::Justification::centredRight);
    }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (area.toNearestInt());
    const auto& grid = curves.grid;
    const auto stroke = [&] (const juce::Path& path, juce::Colour colour, float width)
    {
        if (overImage)   // a dark edge so the curve reads over the spectrogram
        {
            g.setColour (theme::plane.withAlpha (0.85f));
            g.strokePath (path, juce::PathStrokeType (width + 3.0f));
        }
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (width));
    };
    if (curves.showProposal)
    {
        g.setColour (theme::aqua.withAlpha (0.7f));
        strokeDashed (g, curve (grid, curves.proposal, area, true), 1.6f, 5.0f, 4.0f);
    }
    stroke (curve (grid, curves.applied, area, true), theme::aqua.withAlpha (curves.correctionOn ? 1.0f : 0.4f), 2.2f);
    stroke (curve (grid, curves.voicing, area, true), theme::magenta.withAlpha (curves.voicingOn ? 1.0f : 0.4f), 2.0f);
    if (loudnessActive)
        stroke (curve (grid, curves.loudness, area, true), theme::gold, 2.0f);

    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    for (int b = 0; b < roomeq::numVoicingBands; ++b)
    {
        const auto& v = lastEq.voicing[static_cast<std::size_t> (b)];
        const auto c = voicingHandle (b, area);
        const auto circle = juce::Rectangle<float> (2.0f * handleRadius, 2.0f * handleRadius).withCentre (c);
        if (v.on && lastEq.voicingOn)
        {
            g.setColour (theme::magenta);
            g.fillEllipse (circle);
            g.setColour (theme::ink);
        }
        else
        {
            g.setColour (theme::surface);
            g.fillEllipse (circle);
            g.setColour (theme::muted);
            g.drawEllipse (circle, 1.2f);
        }
        g.drawText (juce::String (b + 1), circle, juce::Justification::centred);
        if (b == selectedBand)
        {
            g.setColour (theme::ink);
            g.drawEllipse (circle.expanded (3.0f), 1.4f);
        }
    }
}

void ResponseGraph::drawHover (juce::Graphics& g) const
{
    const auto response = responseArea();
    const auto eq = eqArea();
    const auto inResponse = display != nullptr && response.getHorizontalRange().contains (hoverX);
    if (hoverX < response.getX() || hoverX > response.getRight() || draggingBand >= 0 || draggingPoint >= 0)
        return;
    const auto hz = hzFor (hoverX, response);
    const auto i = nearestIndex (curves.grid, hz);

    g.setColour (theme::muted.withAlpha (0.6f));
    g.drawVerticalLine (juce::roundToInt (hoverX), response.getY(), response.getBottom());
    g.drawVerticalLine (juce::roundToInt (hoverX), eq.getY(), eq.getBottom());

    auto text = theme::formatHz (curves.grid[i]);
    if (inResponse)
    {
        const auto& s = display->summary;
        if (std::isfinite (s.averageDb[i]))
            text << "   average " << juce::String (s.averageDb[i], 1);
        if (! targetDb.empty() && std::isfinite (targetDb[i]))
            text << "   target " << juce::String (targetDb[i], 1);
        if (! predicted.empty() && std::isfinite (predicted[i]))
            text << "   predicted " << juce::String (predicted[i], 1);
        if (! display->verifiedDb.empty() && std::isfinite (display->verifiedDb[i]))
            text << "   verified " << juce::String (display->verifiedDb[i], 1);
        if (const auto* sel = selectedCurve(); sel != nullptr && std::isfinite ((*sel)[i]))
            text << "   " << selectedName() << " " << juce::String ((*sel)[i], 1);
        text << " dB";
    }
    text << "   |   correction " << juce::String (curves.applied[i], 1) << " dB, voicing " << juce::String (curves.voicing[i], 1)
         << " dB";
    if (curves.showLoudness && curves.loudnessOn && curves.loudnessCalibrated)
        text << ", loudness " << juce::String (curves.loudness[i], 1) << " dB";

    g.setFont (juce::FontOptions (12.0f));
    const auto w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 16.0f;
    auto box = juce::Rectangle<float> (hoverX + 8.0f, response.getY() + 8.0f, w, 22.0f);
    if (box.getRight() > response.getRight())
        box.setX (juce::jmax (response.getX(), hoverX - 8.0f - w));
    g.setColour (theme::plane.withAlpha (0.9f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (theme::ink);
    g.drawText (text, box, juce::Justification::centred);
}

void ResponseGraph::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

    const auto response = responseArea();
    if (showMode)
    {
        drawShowChange (g, response);
        drawShowPanel (g, eqArea());
        drawShowHover (g);
        return;
    }
    if (display == nullptr)
    {
        drawFrequencyAxis (g, response, false);
        g.setColour (theme::axis);
        g.drawRect (response, 1.0f);
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("No measurements yet. Connect the mic, then press Measure position.", response,
                    juce::Justification::centred);
    }
    else
    {
        drawResponse (g, response);
    }
    drawEq (g, eqArea());
    drawHover (g);
}

void ResponseGraph::drawShowChange (juce::Graphics& g, juce::Rectangle<float> area) const
{
    drawFrequencyAxis (g, area, false);
    constexpr double range = 6.0;   // bigger changes are pinned to the edge, with their value
    const auto yFor = [&] (double db)
    {
        return area.getY() + static_cast<float> ((range - db) / (2.0 * range)) * area.getHeight();
    };
    g.setFont (juce::FontOptions (11.0f));
    for (int db = -6; db <= 6; db += 3)
    {
        const auto y = yFor (db);
        g.setColour (db == 0 ? theme::axis : theme::grid);
        g.drawHorizontalLine (juce::roundToInt (y), area.getX(), area.getRight());
        g.setColour (theme::muted);
        g.drawText ((db > 0 ? "+" : "") + juce::String (db), juce::Rectangle<float> (0.0f, y - 8.0f, area.getX() - 6.0f, 16.0f),
                    juce::Justification::centredRight);
    }
    g.setColour (theme::axis);
    g.drawRect (area, 1.0f);

    const auto info = processor.getShow().getInfo();
    const auto& st = info.state;
    const auto legendEnd = drawLegend (g, { area.getX(), 8.0f }, { { "Change since soundcheck (tonal)", theme::blue, false },
                                                                   { "Flagged", theme::warning, false },
                                                                   { "3 dB", theme::warning.withAlpha (0.6f), true } });
    auto header = juce::Rectangle<float> (legendEnd + 16.0f, 4.0f, juce::jmax (0.0f, area.getRight() - 4.0f - legendEnd - 16.0f), 20.0f);
    g.setFont (juce::FontOptions (12.0f));
    if (info.hasReference && std::isfinite (st.levelDb))
    {
        const auto r = std::round (st.levelDb * 10.0) / 10.0;
        const auto level = std::abs (r) < 0.05 ? juce::String ("0.0")
                                                : (r > 0.0 ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (r), 1);
        g.setColour (st.levelFlag != 0 ? theme::warning : theme::ink2);
        g.drawText ("Level after the plugin: " + level + " dB", header.removeFromRight (200.0f), juce::Justification::centredRight);
        header.removeFromRight (16.0f);
    }
    if (info.hasReference)   // left out when there's no room: the left panel says it too
    {
        const auto countdown = showCountdown (info);
        if (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), countdown) <= header.getWidth())
        {
            g.setColour (theme::muted);
            g.drawText (countdown, header, juce::Justification::centredRight);
        }
    }

    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion (area.toNearestInt());
    for (const auto db : { -3.0, 3.0 })
    {
        juce::Path line;
        line.startNewSubPath (area.getX(), yFor (db));
        line.lineTo (area.getRight(), yFor (db));
        g.setColour (theme::warning.withAlpha (0.6f));
        strokeDashed (g, line, 1.2f, 5.0f, 4.0f);
    }

    g.setFont (juce::FontOptions (14.0f));
    if (! info.hasReference)
    {
        g.setColour (theme::ink2);
        g.drawFittedText ("No soundcheck reference yet. With music or pink noise playing, press Store reference.",
                          area.reduced (20.0f).toNearestInt(), juce::Justification::centred, 2);
        return;
    }
    const auto& bands = roomeq::recheckBands();
    const auto hovered = showBandAt ({ hoverX, hoverY });
    auto any = false;
    for (std::size_t b = 0; b < bands.size() && b < st.deltaDb.size(); ++b)
    {
        if (static_cast<int> (b) == hovered)
        {
            const auto x0 = xFor (bands[b] * std::exp2 (-1.0 / 6.0), area), x1 = xFor (bands[b] * std::exp2 (1.0 / 6.0), area);
            g.setColour (theme::ink.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (x0, area.getY(), x1 - x0, area.getHeight()));
        }
        const auto x0 = xFor (bands[b] * std::exp2 (-1.0 / 6.0), area), x1 = xFor (bands[b] * std::exp2 (1.0 / 6.0), area);
        const auto w = (x1 - x0) * 0.7f, cx = 0.5f * (x0 + x1);
        const auto d = st.deltaDb[b];
        if (! std::isfinite (d))
        {
            g.setColour (theme::muted.withAlpha (0.5f));
            g.fillRect (juce::Rectangle<float> (cx - w / 2.0f, yFor (0.0) - 1.0f, w, 2.0f));
            continue;
        }
        any = true;
        const auto clamped = juce::jlimit (-range, range, d);
        const auto top = std::min (yFor (clamped), yFor (0.0)), bottom = std::max (yFor (clamped), yFor (0.0));
        const auto flagged = b < st.flags.size() && st.flags[b] != 0;
        g.setColour (flagged ? (std::abs (d) >= 6.0 ? theme::critical : theme::warning) : theme::blue.withAlpha (0.75f));
        g.fillRoundedRectangle (juce::Rectangle<float> (cx - w / 2.0f, top, w, std::max (1.5f, bottom - top)), 2.0f);
        if (std::abs (d) > range)
        {
            // Off the scale: an arrow at the edge, and the value.
            const auto up = d > 0.0;
            const auto tip = up ? top + 3.0f : bottom - 3.0f;
            const auto base = up ? tip + 7.0f : tip - 7.0f;
            juce::Path arrow;
            arrow.addTriangle (cx, tip, cx - 5.0f, base, cx + 5.0f, base);
            g.setColour (theme::plane);
            g.fillPath (arrow);
            if (w >= 22.0f)   // its value, when the bar is wide enough to hold it
            {
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                const auto label = juce::Rectangle<float> (cx - w / 2.0f, up ? base + 1.0f : base - 15.0f, w, 14.0f);
                g.drawFittedText ((up ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::abs (d), 1),
                                  label.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }
        }
    }
    if (! any)
    {
        g.setColour (theme::ink2);
        g.drawFittedText ("Listening: the first result needs about a minute of music the mic hears clearly.",
                          area.reduced (20.0f).toNearestInt(), juce::Justification::centred, 2);
    }
}

// ---------------------------------------------------------------------------
// Interaction

void ResponseGraph::setVoicingParam (int band, const char* what, float value)
{
    if (auto* p = processor.getParameters().getParameter ("v" + juce::String (band + 1) + what))
        p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void ResponseGraph::mouseMove (const juce::MouseEvent& e)
{
    hoverX = static_cast<float> (e.x);
    hoverY = static_cast<float> (e.y);
    const auto overHandle = voicingHandleAt (e.position) >= 0 || targetPointAt (e.position) >= 0;
    setMouseCursor (overHandle ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void ResponseGraph::mouseExit (const juce::MouseEvent&)
{
    hoverX = hoverY = -1.0f;
    repaint();
}

void ResponseGraph::mouseDown (const juce::MouseEvent& e)
{
    draggingBand = voicingHandleAt (e.position);
    if (draggingBand >= 0)
    {
        selectedBand = draggingBand;
        if (onVoicingBandSelected)
            onVoicingBandSelected (draggingBand);
        for (const auto* what : { "Freq", "Gain" })
            if (auto* p = processor.getParameters().getParameter ("v" + juce::String (draggingBand + 1) + what))
                p->beginChangeGesture();
        repaint();
        return;
    }
    draggingPoint = targetPointAt (e.position);
    if (draggingPoint >= 0)
        dragPoints = lastTarget.points;
}

void ResponseGraph::mouseDrag (const juce::MouseEvent& e)
{
    hoverX = static_cast<float> (e.x);
    if (draggingBand >= 0)
    {
        const auto area = eqArea();
        setVoicingParam (draggingBand, "Freq", static_cast<float> (hzFor (e.position.x, area)));
        if (roomeq::hasGain (lastEq.voicing[static_cast<std::size_t> (draggingBand)].type))
            setVoicingParam (draggingBand, "Gain", static_cast<float> (juce::jlimit (-18.0, 18.0, eqDbFor (e.position.y, area))));
        refresh();
        return;
    }
    if (draggingPoint >= 0 && customTargetEditable())
    {
        const auto area = responseArea();
        const auto i = static_cast<std::size_t> (draggingPoint);
        auto lo = fMin, hi = fMax;
        const auto step = std::exp2 (1.0 / 48.0);
        if (i > 0)
            lo = dragPoints[i - 1].first * step;
        if (i + 1 < dragPoints.size())
            hi = dragPoints[i + 1].first / step;
        dragPoints[i].first = juce::jlimit (lo, juce::jmax (lo, hi), hzFor (e.position.x, area));
        dragPoints[i].second = juce::jlimit (-24.0, 24.0, dbFor (e.position.y, area) - targetOffset);
        auto t = processor.getCustomTarget();
        t.points = dragPoints;
        processor.setCustomTarget (t);
        refresh();
    }
}

void ResponseGraph::mouseUp (const juce::MouseEvent&)
{
    if (draggingBand >= 0)
        for (const auto* what : { "Freq", "Gain" })
            if (auto* p = processor.getParameters().getParameter ("v" + juce::String (draggingBand + 1) + what))
                p->endChangeGesture();
    draggingBand = -1;
    draggingPoint = -1;
    repaint();
}

void ResponseGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto band = voicingHandleAt (e.position); band >= 0)
    {
        setVoicingParam (band, "On", lastEq.voicing[static_cast<std::size_t> (band)].on ? 0.0f : 1.0f);
        refresh();
        return;
    }
    if (! customTargetEditable() || ! responseArea().contains (e.position))
        return;
    auto t = processor.getCustomTarget();
    if (const auto point = targetPointAt (e.position); point >= 0)
    {
        if (t.points.size() > 1)
            t.points.erase (t.points.begin() + point);
    }
    else
    {
        const auto area = responseArea();
        t.points.push_back ({ hzFor (e.position.x, area), dbFor (e.position.y, area) - targetOffset });
    }
    processor.setCustomTarget (t);
    refresh();
}

void ResponseGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    auto band = voicingHandleAt (e.position);
    if (band < 0 && eqArea().contains (e.position))
        band = selectedBand;
    if (band < 0)
        return;
    const auto& v = lastEq.voicing[static_cast<std::size_t> (band)];
    if (! roomeq::hasQ (v.type))
        return;
    const auto q = juce::jlimit (0.3, 8.0, v.q * std::exp2 (static_cast<double> (wheel.deltaY) * 2.0));
    setVoicingParam (band, "Q", static_cast<float> (q));
    refresh();
}
