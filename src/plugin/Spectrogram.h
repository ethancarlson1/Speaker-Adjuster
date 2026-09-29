#pragma once

#include "plugin/SampleFifo.h"

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

// The show view's spectrogram of the mic: frequency across, on the graph's
// 20 Hz-20 kHz log axis (the EQ curves are drawn over it, and the change bars
// above it line up), time down with the newest at the top, about 20 s of it.
//
// Each column is the energy in its slice of the axis (the power density there
// times its width, like an RTA), so pink noise reads flat. Colour is level against the
// loudest of the last few seconds, over 60 dB. The graph that draws it runs
// update() on the message thread while it's on screen; the audio thread just
// fills the FIFO.
class Spectrogram final
{
public:
    static constexpr int fftOrder = 13;              // 8192 points: ~6 Hz bins at 48 kHz, 170 ms
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hop = fftSize / 4;           // ~23 frames a second at 48 kHz
    static constexpr double historySeconds = 20.0;
    static constexpr float rangeDb = 60.0f;
    static constexpr double fMin = 20.0, fMax = 20000.0;

    Spectrogram (SampleFifo& fifoToUse, std::function<double()> sampleRateSource);

    // The picture's size in pixels (what's there is kept, scaled).
    void setSize (int width, int height);
    // Takes what's new in the FIFO and analyses it.
    void update();
    // Draws it into `area` (the size given to setSize).
    void draw (juce::Graphics&, juce::Rectangle<int> area, float opacity) const;
    bool hearsTheMic() const;   // analysed something in the last 2 s
    int getFramesAnalysed() const { return frames; }
    double getNewestPeakHz() const { return newestPeakHz; }   // the loudest column of the newest frame

    static juce::Colour colourFor (float normalised);          // 0 (quiet) .. 1 (loudest)

private:
    void analyseFrame();
    void mapColumns();
    void writeRow();

    SampleFifo& fifo;
    std::function<double()> sampleRate;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> window, history, work, pulled;
    int historyPos = 0, sinceFrame = 0, filled = 0;
    double fs = 0.0;

    // Display.
    juce::Image image;
    std::vector<int> binLo, binHi;          // per column, inclusive; binHi < binLo: between bins
    std::vector<double> centreBin, colWidthBins;
    std::vector<float> rowLevels;           // dB, the loudest of the frames in the row being built
    double rowSeconds = 0.05, rowFilled = 0.0;
    int writeRowIndex = 0;
    float reference = -200.0f;              // dB, the loudest recently
    std::array<juce::PixelARGB, 256> lut;
    int frames = 0;
    double newestPeakHz = 0.0;
    juce::uint32 lastDataMs = 0;
};
