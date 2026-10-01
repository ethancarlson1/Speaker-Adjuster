#pragma once

#include "plugin/MeasurementEngine.h"
#include "roomeq/quality.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

// Measurement Quality (Measure page): the selected capture's signal-to-noise,
// coherence (music and noise), repeatability (repeat sweeps), usable range and
// measurement confidence, in words. With Advanced on, each rating shows the
// number behind it, and Details... opens the raw numbers: each octave band's
// level, SNR, coherence and repeat spread, the arrival, and a sweep's impulse
// response around it.
class QualityCard final : public juce::Component
{
public:
    QualityCard();

    // The capture to show (none: a hint), judged against the zone's reference band.
    void setEntry (std::optional<MeasurementEngine::Entry> entry, std::pair<double, double> referenceBand);
    const std::optional<roomeq::MeasurementQuality>& getQuality() const { return quality; }
    void setAdvanced (bool on);              // the numbers behind each rating, and Details...

    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int minimumHeight = 116;     // the rows, tight (the smallest window)
    static constexpr int preferredHeight = 200;   // with the subtitle and a line of reasons

    // Tests: the button, and what it opens.
    juce::TextButton& getDetailsButton() { return detailsButton; }
    std::unique_ptr<juce::Component> createDetails() const;

private:
    std::optional<MeasurementEngine::Entry> entry;
    std::pair<double, double> band { 250.0, 4000.0 };
    std::optional<roomeq::MeasurementQuality> quality;
    bool advanced = false;
    juce::TextButton detailsButton { juce::String::fromUTF8 ("Details\xe2\x80\xa6") };
};

// The raw numbers behind a capture's quality.
class QualityDetails final : public juce::Component
{
public:
    QualityDetails (MeasurementEngine::Entry entry, roomeq::MeasurementQuality quality);
    void paint (juce::Graphics&) override;

private:
    void drawImpulse (juce::Graphics&, juce::Rectangle<int> area) const;

    MeasurementEngine::Entry entry;
    roomeq::MeasurementQuality quality;
};
