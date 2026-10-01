#pragma once

#include "roomeq/eqexport.h"
#include "roomeq/refit.h"
#include "roomeq/x32.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>

// Export the EQ (Correct page, Export...): text to copy (for typing into a
// console), a CSV or JSON file, or a Behringer X32 / Midas M32 snippet for one
// of its buses, matrices or mains.
//
// For an output with only so many bands, the correction can be fitted to 8, 6
// or 4 (roomeq/refit.h); X32/M32 always gets six bells on the desk's own steps
// (roomeq/x32.h). Fits run in the background. A graph shows the correction as
// it plays and what the export plays, with the largest difference between them.
class ExportPanel final : public juce::Component, private juce::Timer
{
public:
    enum class Format { text = 0, csv, json, x32 };
    struct Options
    {
        Format format = Format::text;
        int bands = 0;                                   // 0: all, as played; else at most 8, 6 or 4
        roomeq::x32::Destination destination;
    };
    static constexpr int bandChoices[] = { 8, 6, 4 };

    ExportPanel (roomeq::EqExport asPlayed, Options options);
    ~ExportPanel() override;   // waits for a fit still running (a fraction of a second)

    std::function<void (const Options&)> onOptionsChanged;   // to remember them
    // Copy / Save... pressed: the options, what to write, and a few words for the notice
    // ("fitted to 6 bands, within 1.7 dB"; empty when exported as played).
    std::function<void (const Options&, const std::string& content, const juce::String& summary)> onExport;

    const Options& getOptions() const { return options; }
    void setOptions (const Options& o);
    bool isFitting() const;                          // the current choice's fit isn't done yet
    void waitForFit();                               // tests: until it is
    std::optional<std::string> getContent() const;  // what Copy / Save writes now (none while fitting)
    juce::String getSummary() const;
    juce::String getFitText() const;                 // the line under the graph
    juce::TextButton& getExportButton() { return exportButton; }

    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int preferredWidth = 580, preferredHeight = 440;

private:
    void timerCallback() override;
    void readChoice (juce::ComboBox& box);
    void startFit();
    void refresh();
    std::optional<roomeq::Refit> refitFor (int bands) const;
    std::optional<roomeq::x32::Fit> deskFit() const;
    std::optional<roomeq::EqExport> exportData() const;   // the export with the chosen fit
    int correctionBands() const { return static_cast<int> (base.correction.size()); }

    roomeq::EqExport base;
    Options options;
    std::vector<double> grid, playedDb, exportDb;
    std::map<int, std::shared_future<roomeq::Refit>> refits;
    std::shared_future<roomeq::x32::Fit> desk;

    juce::Label formatLabel { {}, "Export as" }, bandsLabel { {}, "Correction bands" }, stripLabel { {}, "X32 / M32 strip" };
    juce::ComboBox formatBox, bandsBox, stripBox;
    juce::TextButton exportButton;
    juce::Rectangle<int> graphArea, textArea;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportPanel)
};
