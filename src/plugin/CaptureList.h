#pragma once

#include "plugin/MeasurementEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// The list of captured positions: grade badge, name (double-click to rename),
// type and the direct sound's arrival (a loop delay, with a dot for its
// confidence: green high, amber medium, red low), the grading reasons, and
// include / redo / delete.
// Clicking a row (its name too) selects it and highlights its curve on the
// graph; clicking the selected row again clears the selection.
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

    // appliedId: verify captures taken with another correction are marked as older.
    void setEntries (std::vector<MeasurementEngine::Entry> newEntries, bool measuring, int appliedId = 0);
    int getSelectedId() const;
    void selectId (int id);                   // -1: none (as a click would, it tells onSelect)
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class Row;

    int getNumRows() override;
    void paintListBoxItem (int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow (int row, bool selected, juce::Component* existing) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void selectRow (int row);                 // from a click on a row's name

    Callbacks callbacks;
    juce::ListBox list { "Captures", this };
    std::vector<MeasurementEngine::Entry> entries;
    bool measuring = false;
    int appliedId = 0;
    bool justSelected = false;                // this click selected the row (so it doesn't also clear it)
};
