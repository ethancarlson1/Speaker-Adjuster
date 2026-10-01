#pragma once

#include "plugin/MeasurementEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

// System Summary (Correct tab, where the capture list is): the measurements in
// a few lines (confidence, how much the positions differ, usable bandwidth,
// the largest broad issue), the proposal (filters, largest cut and boost,
// nulls left alone, expected improvement), and every place the correction
// was held back, with the reason. An overview; the graph has the detail.
class SystemSummaryPanel final : public juce::Component
{
public:
    SystemSummaryPanel();

    void setDisplay (std::shared_ptr<const MeasurementEngine::Display> display);
    const std::optional<roomeq::SystemSummary>& getSummary() const { return summary; }
    juce::String getWhyText() const { return why.getText(); }   // tests

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    std::shared_ptr<const MeasurementEngine::Display> shown;
    std::optional<roomeq::SystemSummary> summary;
    juce::TextEditor why;   // the reasons, scrollable when there are many
    juce::Rectangle<int> measuredArea, proposalArea, whyHeading;
    int rowHeight = 20;
};
