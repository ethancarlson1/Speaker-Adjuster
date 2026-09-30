#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

// The alignment assistant (Zone tab): arrival times turned into a delay to apply.
//
// Pick the main system's measurement (from another instance of the plugin in
// this DAW, or type its arrival) and this zone's measurement from the same
// spot; it suggests this zone's delay, applied only when the engineer presses
// Apply. The system latency (a loopback, or typed) turns arrivals into flight
// times; the suggestion doesn't need it when both zones share an interface.
//
// A Subs zone lines up by phase instead: the mains' sweep and the sub's from
// the same spot predict how they sum over the crossover, and it suggests the
// sub's delay and polarity (both set by Apply). That needs the mains' instance
// in this DAW: a response can't be typed in.
class AlignmentPanel final : public juce::Component,
                             private juce::Timer
{
public:
    explicit AlignmentPanel (AdaptiveRoomEQProcessor&);
    ~AlignmentPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int preferredHeight = 256;

    // The current suggestion (none until both arrivals are known), and what the panel says. For tests too.
    std::optional<roomeq::DelaySuggestion> getSuggestion() const { return suggestion; }
    std::optional<roomeq::SubAlignment> getSubSuggestion() const { return subSuggestion; }
    const juce::String& getResultText() const { return result; }
    // Tests: choose what to compare, and apply.
    juce::ComboBox& getWithBox() { return withBox; }
    juce::ComboBox& getTheirBox() { return theirBox; }
    juce::ComboBox& getOwnBox() { return ownBox; }
    juce::TextEditor& getTypedArrival() { return typedArrival; }
    juce::TextButton& getApplyButton() { return applyButton; }
    void refresh();                                   // the timer does this; tests can call it

private:
    void timerCallback() override { refresh(); }
    void rebuildLists();
    void update();
    void updateSub();
    bool subMode() const { return processor.getZone() == AdaptiveRoomEQProcessor::Zone::subs; }
    int typedId() const { return subMode() ? -1 : static_cast<int> (zones.size()) + 1; }   // none for subs

    AdaptiveRoomEQProcessor& processor;
    juce::Label heading, latencyLabel, withLabel, theirLabel, ownLabel;
    juce::TextEditor latencyValue, typedArrival;
    juce::TextButton loopbackButton { "Loopback" }, applyButton { "Apply" };
    juce::ComboBox withBox, theirBox, ownBox;

    std::vector<ZoneRegistry::Zone> zones;            // the other instances, as listed in withBox
    std::vector<MeasurementEngine::Entry> own;        // this zone's measurements, as listed in ownBox (newest first)
    juce::String listedFingerprint;
    std::optional<roomeq::DelaySuggestion> suggestion;
    std::optional<roomeq::SubAlignment> subSuggestion;
    juce::String subInputs;                           // what subSuggestion was worked out from (it takes a few ms)
    juce::String result;
    juce::Rectangle<int> resultBounds;
};
