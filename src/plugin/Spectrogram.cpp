#include "plugin/Spectrogram.h"

#include "plugin/Theme.h"

#include <cmath>

Spectrogram::Spectrogram (SampleFifo& fifoToUse, std::function<double()> sampleRateSource)
    : fifo (fifoToUse), sampleRate (std::move (sampleRateSource))
{
    window.resize (fftSize);
    juce::dsp::WindowingFunction<float>::fillWindowingTables (window.data(), fftSize,
                                                             juce::dsp::WindowingFunction<float>::hann, false);
    history.assign (fftSize, 0.0f);
    work.assign (2 * fftSize, 0.0f);
    pulled.assign (1 << 15, 0.0f);
    for (std::size_t i = 0; i < lut.size(); ++i)
        lut[i] = colourFor (static_cast<float> (i) / static_cast<float> (lut.size() - 1)).getPixelARGB();
    setInterceptsMouseClicks (false, false);
    setOpaque (true);
}

juce::Colour Spectrogram::colourFor (float t)
{
    // Near-black, blue, aqua, gold, pale: quiet to loud.
    static const std::array<std::pair<float, juce::Colour>, 5> stops { {
        { 0.0f, theme::plane },
        { 0.35f, juce::Colour (0xff1d3f7a) },
        { 0.6f, theme::blue },
        { 0.8f, theme::aqua },
        { 1.0f, juce::Colour (0xfff5e3a0) },
    } };
    t = juce::jlimit (0.0f, 1.0f, t);
    for (std::size_t i = 1; i < stops.size(); ++i)
        if (t <= stops[i].first)
            return stops[i - 1].second.interpolatedWith (stops[i].second,
                                                         (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first));
    return stops.back().second;
}

void Spectrogram::visibilityChanged()
{
    // Made visible before the editor has a window (a session that opens in the show
    // view) still needs its timer, so it runs while visible and skips work off screen.
    if (isVisible())
        startTimerHz (30);
    else
        stopTimer();
}

void Spectrogram::resized()
{
    const auto w = juce::jmax (1, getWidth()), h = juce::jmax (1, getHeight());
    const auto old = image;
    image = juce::Image (juce::Image::RGB, w, h, true);
    image.clear (image.getBounds(), theme::plane);
    if (old.isValid())
    {
        // Keep what's there, scaled: unrolled so the newest row is row 0.
        juce::Graphics g (image);
        const auto oh = old.getHeight(), top = oh - writeRowIndex;
        const auto scaleY = static_cast<float> (h) / static_cast<float> (oh);
        g.drawImage (old, 0, 0, w, juce::roundToInt (static_cast<float> (top) * scaleY), 0, writeRowIndex, old.getWidth(), top);
        if (writeRowIndex > 0)
            g.drawImage (old, 0, juce::roundToInt (static_cast<float> (top) * scaleY), w,
                         h - juce::roundToInt (static_cast<float> (top) * scaleY), 0, 0, old.getWidth(), writeRowIndex);
    }
    writeRowIndex = 0;
    rowSeconds = historySeconds / h;
    rowFilled = 0.0;
    rowLevels.assign (static_cast<std::size_t> (w), -300.0f);
    mapColumns();
}

void Spectrogram::mapColumns()
{
    const auto w = image.isValid() ? image.getWidth() : 0;
    binLo.assign (static_cast<std::size_t> (w), 0);
    binHi.assign (static_cast<std::size_t> (w), -1);
    centreBin.assign (static_cast<std::size_t> (w), 0.0);
    colWidthBins.assign (static_cast<std::size_t> (w), 1.0);
    if (fs <= 0.0)
        return;
    const auto perHz = fftSize / fs;
    for (int x = 0; x < w; ++x)
    {
        const auto lo = fMin * std::pow (fMax / fMin, static_cast<double> (x) / w);
        const auto hi = fMin * std::pow (fMax / fMin, static_cast<double> (x + 1) / w);
        const auto i = static_cast<std::size_t> (x);
        binLo[i] = static_cast<int> (std::ceil (lo * perHz));
        binHi[i] = juce::jmin (fftSize / 2, static_cast<int> (std::ceil (hi * perHz)) - 1);
        centreBin[i] = std::sqrt (lo * hi) * perHz;
        colWidthBins[i] = (hi - lo) * perHz;
    }
}

void Spectrogram::timerCallback()
{
    if (! isShowing())
        return;
    update();
    repaint();
}

void Spectrogram::update()
{
    const auto rate = sampleRate ? sampleRate() : 0.0;
    if (rate > 0.0 && std::abs (rate - fs) > 0.5)
    {
        fs = rate;
        mapColumns();
    }
    if (fs <= 0.0)
        return;
    for (;;)
    {
        const auto n = fifo.pull (pulled.data(), static_cast<int> (pulled.size()));
        if (n == 0)
            break;
        lastDataMs = juce::Time::getMillisecondCounter();
        for (int i = 0; i < n; ++i)
        {
            history[static_cast<std::size_t> (historyPos)] = pulled[static_cast<std::size_t> (i)];
            historyPos = (historyPos + 1) % fftSize;
            filled = juce::jmin (fftSize, filled + 1);
            if (++sinceFrame >= hop && filled == fftSize)
            {
                sinceFrame = 0;
                analyseFrame();
            }
        }
    }
}

void Spectrogram::analyseFrame()
{
    for (int i = 0; i < fftSize; ++i)
        work[static_cast<std::size_t> (i)] = history[static_cast<std::size_t> ((historyPos + i) % fftSize)] * window[static_cast<std::size_t> (i)];
    std::fill (work.begin() + fftSize, work.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (work.data(), true);   // magnitudes, bins 0..N/2

    const auto power = [&] (int k) { return static_cast<double> (work[static_cast<std::size_t> (k)]) * work[static_cast<std::size_t> (k)]; };
    auto frameMax = -300.0f;
    auto peakColumn = 0;
    for (std::size_t x = 0; x < rowLevels.size(); ++x)
    {
        // The power density in the column (per bin) times its width in bins: the sum of
        // its bins when it's wide, and no stripes where it's narrower than a bin.
        double density = 0.0;
        if (binHi[x] >= binLo[x])
        {
            for (int k = binLo[x]; k <= binHi[x]; ++k)
                density += power (k);
            density /= binHi[x] - binLo[x] + 1;
        }
        else
        {
            const auto c = juce::jlimit (0.0, fftSize / 2.0 - 1.0, centreBin[x]);
            const auto k = static_cast<int> (c);
            const auto f = c - k;
            density = (1.0 - f) * power (k) + f * power (k + 1);
        }
        const auto p = density * colWidthBins[x];
        const auto db = static_cast<float> (10.0 * std::log10 (p + 1e-30));
        rowLevels[x] = juce::jmax (rowLevels[x], db);
        if (db > frameMax)
        {
            frameMax = db;
            peakColumn = static_cast<int> (x);
        }
    }
    const auto seconds = static_cast<double> (hop) / fs;
    reference = juce::jmax (frameMax, reference - static_cast<float> (3.0 * seconds));   // follows the loudest, drops 3 dB/s
    newestPeakHz = rowLevels.empty() ? 0.0 : fMin * std::pow (fMax / fMin, (peakColumn + 0.5) / static_cast<double> (rowLevels.size()));
    ++frames;

    rowFilled += seconds;
    while (rowFilled >= rowSeconds)
    {
        writeRow();
        rowFilled -= rowSeconds;
    }
}

void Spectrogram::writeRow()
{
    if (! image.isValid())
        return;
    // Newest at the top: rows are written upwards through a circular image.
    writeRowIndex = (writeRowIndex - 1 + image.getHeight()) % image.getHeight();
    juce::Image::BitmapData data (image, 0, writeRowIndex, image.getWidth(), 1, juce::Image::BitmapData::writeOnly);
    const auto floor = reference - rangeDb;
    for (int x = 0; x < image.getWidth(); ++x)
    {
        const auto t = (rowLevels[static_cast<std::size_t> (x)] - floor) / rangeDb;
        const auto last = static_cast<int> (lut.size()) - 1;
        const auto i = static_cast<std::size_t> (juce::jlimit (0, last, juce::roundToInt (t * static_cast<float> (last))));
        data.setPixelColour (x, 0, juce::Colour (lut[i]));
    }
    std::fill (rowLevels.begin(), rowLevels.end(), -300.0f);
}

void Spectrogram::paint (juce::Graphics& g)
{
    g.fillAll (theme::plane);
    if (image.isValid())
    {
        // The circular image in two slices: from the newest row down to the bottom, then the rest.
        const auto h = image.getHeight(), w = image.getWidth();
        const auto top = h - writeRowIndex;
        g.drawImage (image, 0, 0, w, top, 0, writeRowIndex, w, top);
        if (writeRowIndex > 0)
            g.drawImage (image, 0, top, w, writeRowIndex, 0, 0, w, writeRowIndex);
    }
    if (frames == 0 || juce::Time::getMillisecondCounter() - lastDataMs > 2000)
    {
        g.setColour (theme::ink2);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Waiting for the mic", getLocalBounds(), juce::Justification::centred);
    }
}
