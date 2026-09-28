#pragma once

#include "plugin/MeasurementEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// The list of captured positions: grade badge, name (double-click to rename),
// type and loop delay, the grading reasons, and include / redo / delete.
class CaptureList final : public juce::Component,
                          private juce::ListBoxModel
{
public:
    struct Callbacks
    {
        std::function<void (int id)> onSelect;
        std::function<void (int id, const juce::String&)> onRename;
        std::function<void (int id, bool include)> onInclude;
        std::function<void (int id)> onRedo;
        std::function<void (int id)> onDelete;
    };

    explicit CaptureList (Callbacks callbacks);

    void setEntries (std::vector<MeasurementEngine::Entry> newEntries, bool measuring);
    int getSelectedId() const;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Row;

    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
    void selectedRowsChanged (int lastRowSelected) override;

    Callbacks callbacks;
    juce::ListBox list { "Captures", this };
    std::vector<MeasurementEngine::Entry> entries;
    bool measuring = false;
};
