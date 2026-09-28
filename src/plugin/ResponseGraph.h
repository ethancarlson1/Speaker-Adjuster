#pragma once

#include "plugin/MeasurementEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

// Log-frequency response graph: each position (level-aligned), the power
// average, and the flat target over the PA's usable range. Hovering shows a
// readout at the cursor frequency.
class ResponseGraph final : public juce::Component
{
public:
    void setData (std::shared_ptr<const MeasurementEngine::Display> display, int selectedId);

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> plotArea() const;
    float xFor (double hz, juce::Rectangle<float> area) const;
    float yFor (double db, juce::Rectangle<float> area) const;
    juce::Path curve (const std::vector<double>& db, juce::Rectangle<float> area) const;
    void drawAxes (juce::Graphics&, juce::Rectangle<float> area) const;
    void drawLegend (juce::Graphics&, juce::Rectangle<float> area) const;
    void drawHover (juce::Graphics&, juce::Rectangle<float> area) const;
    int selectedIndex() const;

    std::shared_ptr<const MeasurementEngine::Display> display;
    int selectedId = -1;
    double dbTop = 10.0, dbBottom = -30.0;
    float hoverX = -1.0f;
};
