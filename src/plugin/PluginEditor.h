#pragma once

#include "plugin/PluginProcessor.h"

// Placeholder UI: bus status and input/output meters, so routing can be
// checked in a host. The capture list and response graph replace this later.
class AdaptiveRoomEQEditor final : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit AdaptiveRoomEQEditor (AdaptiveRoomEQProcessor&);
    ~AdaptiveRoomEQEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawMeter (juce::Graphics&, juce::Rectangle<int> area, const juce::String& label, float levelDb) const;

    AdaptiveRoomEQProcessor& processor;
    float micLevelDb = -100.0f;
    float outputLevelDb = -100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveRoomEQEditor)
};
