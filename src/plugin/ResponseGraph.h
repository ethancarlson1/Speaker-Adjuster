#pragma once

#include "plugin/EqStages.h"
#include "plugin/MeasurementEngine.h"
#include "plugin/Spectrogram.h"

#include <juce_gui_basics/juce_gui_basics.h>

class AdaptiveRoomEQProcessor;

// The response graph. Top: each position (level-aligned), the power average,
// the target, the predicted result with correction and voicing, and the
// verified result (measured through the EQ). Bottom strip: the correction
// (applied and proposed), the voicing EQ and the loudness compensation at the
// current level, with a draggable handle per
// voicing band (drag: frequency and gain, wheel: Q, double-click: on/off).
// With the custom target selected, its points can be dragged too
// (double-click adds or removes a point). Hovering shows a readout.
class ResponseGraph final : public juce::Component,
                            private juce::Timer
{
public:
    explicit ResponseGraph (AdaptiveRoomEQProcessor& processorToUse);

    void setData (std::shared_ptr<const MeasurementEngine::Display> display, int selectedId);
    void refresh();                                   // polled: repaints if the EQ or target changed

    // Show view: instead of the measurements, the mic's spectrogram with the EQ
    // curves and voicing handles over it, or either alone (Both / Spectrogram /
    // EQ), on the same frequency axis, with a readout on hover.
    void setShowMode (bool shouldShow);
    bool isShowMode() const { return showMode; }
    void showPanelChanged();                          // the processor's choice changed (tests, session restore)
    Spectrogram& getSpectrogram() { return spectrogram; }
    juce::String showHoverText (juce::Point<float> p) const;   // what hovering there says ("" for nothing), for tests
    std::function<void (int band)> onVoicingBandSelected;
    void setSelectedVoicingBand (int band);

    // Whether a selected capture's curve is highlighted, for tests.
    bool isHighlighting() const { return selectedCurve() != nullptr; }

    // Where the handles are drawn (component coordinates), for tests.
    juce::Point<float> getVoicingHandlePosition (int band) const { return voicingHandle (band, eqArea()); }
    juce::Point<float> getTargetPointPosition (int index) const { return targetPoint (static_cast<std::size_t> (index), responseArea()); }
    juce::Point<float> getShowPanelPoint (double hz) const;    // in the spectrogram/EQ panel, halfway down

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Curves
    {
        std::vector<double> grid;
        std::vector<double> applied, proposal, voicing, loudness;   // dB
        bool showProposal = false;
        bool correctionOn = true, voicingOn = true;
        bool showLoudness = false, loudnessOn = true, loudnessCalibrated = false;
        bool levelMatch = true;
        float makeupDb = 0.0f;
    };

    juce::Rectangle<float> responseArea() const;
    juce::Rectangle<float> eqArea() const;            // show mode: the spectrogram/EQ panel
    void drawShowPanel (juce::Graphics&, juce::Rectangle<float> area) const;
    void drawShowHover (juce::Graphics&) const;
    bool showsSpectrogram() const;
    bool showsEq() const;
    void timerCallback() override;
    float xFor (double hz, juce::Rectangle<float> area) const;
    double hzFor (float x, juce::Rectangle<float> area) const;
    float yFor (double db, juce::Rectangle<float> area) const;
    double dbFor (float y, juce::Rectangle<float> area) const;
    float eqYFor (double db, juce::Rectangle<float> area) const;
    double eqDbFor (float y, juce::Rectangle<float> area) const;
    juce::Path curve (const std::vector<double>& grid, const std::vector<double>& db, juce::Rectangle<float> area,
                      bool eq) const;
    void drawFrequencyAxis (juce::Graphics&, juce::Rectangle<float> area, bool labels) const;
    void drawLevelAxis (juce::Graphics&, juce::Rectangle<float> area) const;
    void drawEqAxis (juce::Graphics&, juce::Rectangle<float> area) const;
    struct LegendItem { juce::String label; juce::Colour colour; bool dashed; };
    static float drawLegend (juce::Graphics&, juce::Point<float> at, const std::vector<LegendItem>& items);   // returns where it ends
    void drawHover (juce::Graphics&) const;
    void drawResponse (juce::Graphics&, juce::Rectangle<float> area) const;
    // overImage: halos so the curves read over the spectrogram; reserveRight: room kept at the legend row's end.
    void drawEq (juce::Graphics&, juce::Rectangle<float> area, bool overImage = false, float reserveRight = 0.0f) const;
    int selectedIndex() const;                          // among the fit positions, or -1
    const std::vector<double>* selectedCurve() const;   // the selected capture's curve (position or verify), or null
    juce::String selectedName() const;
    bool customTargetEditable() const;

    // Voicing handles and target points, in component coordinates.
    juce::Point<float> voicingHandle (int band, juce::Rectangle<float> area) const;
    int voicingHandleAt (juce::Point<float> p) const;
    juce::Point<float> targetPoint (std::size_t index, juce::Rectangle<float> area) const;
    int targetPointAt (juce::Point<float> p) const;
    void setVoicingParam (int band, const char* what, float value);
    void updateCurves();

    AdaptiveRoomEQProcessor& processor;
    std::shared_ptr<const MeasurementEngine::Display> display;
    Curves curves;
    std::vector<double> predicted;
    std::vector<double> targetDb;     // the current target, anchored on the average (no wait for the fit)
    double targetOffset = 0.0;
    int selectedId = -1;
    double dbTop = 10.0, dbBottom = -30.0;
    static constexpr double eqTop = 12.0, eqBottom = -15.0;
    float hoverX = -1.0f, hoverY = -1.0f;

    // Inputs of the cached curves.
    EqSettings lastEq;
    std::vector<roomeq::Band> lastApplied;
    std::shared_ptr<const MeasurementEngine::Display> lastDisplay;
    roomeq::TargetCurve lastTarget;
    bool lastComparing = false;
    std::vector<roomeq::Band> lastLoudness;
    bool lastLoudnessOn = true, lastLoudnessCalibrated = false;

    bool showMode = false;
    int selectedBand = 0;
    int draggingBand = -1;
    int draggingPoint = -1;
    std::vector<std::pair<double, double>> dragPoints;

    Spectrogram spectrogram;
    juce::TextButton bothButton { "Both" }, spectrogramButton { "Spectrogram" }, eqOnlyButton { "EQ" };
};
