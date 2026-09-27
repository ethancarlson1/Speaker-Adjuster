#include "plugin/PluginEditor.h"

namespace
{
constexpr float meterFloorDb = -80.0f;
constexpr float meterDecayDbPerTick = 1.5f;
constexpr int refreshHz = 30;
} // namespace

AdaptiveRoomEQEditor::AdaptiveRoomEQEditor (AdaptiveRoomEQProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setSize (560, 300);
    startTimerHz (refreshHz);
}

AdaptiveRoomEQEditor::~AdaptiveRoomEQEditor()
{
    stopTimer();
}

void AdaptiveRoomEQEditor::timerCallback()
{
    const auto decayed = [] (float current, float peak)
    {
        const auto peakDb = juce::Decibels::gainToDecibels (peak, meterFloorDb);
        return juce::jmax (peakDb, current - meterDecayDbPerTick, meterFloorDb);
    };

    micLevelDb = decayed (micLevelDb, processor.takeMicPeak());
    outputLevelDb = decayed (outputLevelDb, processor.takeOutputPeak());
    repaint();
}

void AdaptiveRoomEQEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1c1f24));

    auto area = getLocalBounds().reduced (20);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText (JucePlugin_Name, area.removeFromTop (30), juce::Justification::centredLeft);

    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Phase 1 scaffold: audio passes through unchanged.",
                area.removeFromTop (20), juce::Justification::centredLeft);

    area.removeFromTop (16);

    const auto micEnabled = processor.isMicBusEnabled();
    g.setColour (micEnabled ? juce::Colour (0xff7fd18b) : juce::Colour (0xffe0a84f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (micEnabled ? "Measurement mic sidechain: connected"
                           : "Measurement mic sidechain: not connected (route the mic to the sidechain input)",
                area.removeFromTop (22), juce::Justification::centredLeft);

    area.removeFromTop (20);
    drawMeter (g, area.removeFromTop (28), "Mic in", micLevelDb);
    area.removeFromTop (12);
    drawMeter (g, area.removeFromTop (28), "Output", outputLevelDb);
}

void AdaptiveRoomEQEditor::drawMeter (juce::Graphics& g, juce::Rectangle<int> area,
                                      const juce::String& label, float levelDb) const
{
    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (label, area.removeFromLeft (70), juce::Justification::centredLeft);

    const auto readout = area.removeFromRight (80);
    g.drawText (levelDb <= meterFloorDb ? juce::String ("-inf dBFS")
                                        : juce::String (levelDb, 1) + " dBFS",
                readout, juce::Justification::centredRight);

    const auto bar = area.reduced (0, 6).toFloat();
    g.setColour (juce::Colour (0xff2e333b));
    g.fillRoundedRectangle (bar, 3.0f);

    const auto fraction = juce::jmap (levelDb, meterFloorDb, 0.0f, 0.0f, 1.0f);
    g.setColour (levelDb > -3.0f ? juce::Colour (0xffe05f5f) : juce::Colour (0xff5fa8e0));
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, fraction)), 3.0f);
}

void AdaptiveRoomEQEditor::resized()
{
}
