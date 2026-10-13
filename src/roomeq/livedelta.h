#pragma once

// Live delta: the mic against the console's signal, measured from the show.
// Port of prototype/roomeq/livedelta.py.
//
// A running dual-FFT transfer function between x = the console's signal as it
// reaches the plugin (before any of its EQ) and y = the measurement mic, for
// the show view's Delta panel. Above 0 dB the mic hears more of a frequency
// than the console sends, relative to the reference band; below, less.
//
// - Delay: GCC-PHAT (estimateDelay) on the last 4 s of both, every 5 s. A delay
//   is taken only when the two halves of those 4 s agree with it within 0.5 ms;
//   a new one resets the averages. Nothing is averaged until there is one.
// - Frames: Hann-windowed, 1 s rounded up to a power of two, one every 0.5 s:
//   the mic's samples up to the frame's end, the console's from `delay`
//   earlier. A frame whose console part is quieter than -70 dBFS RMS isn't
//   counted, so pauses in the music leave the average as it was.
// - Average: Gxx, Gyy and Gxy per bin, exponential with a 10 s time constant.
// - Display: on 24 points per octave, 20 Hz-20 kHz, smoothed to 1/6 octave:
//   |H|^2 weighted by Gxx (the coherent mic power over the console's power in
//   each slice), and the coherence weighted the same way. Points with
//   coherence under 0.5 are unreliable (crowd, the stage, notes the music isn't
//   playing). 0 dB is the mean of the reliable points in the reference band.
//
// Frames and delay searches happen at fixed sample positions, so the result
// doesn't depend on how the samples arrive. Not real-time safe (it allocates
// and runs FFTs): fed from the message thread.

#include "roomeq/fft.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace roomeq
{
struct LiveDeltaConfig
{
    double fftSeconds = 1.0;          // rounded up to a power of two at the session rate
    double hopSeconds = 0.5;
    double averageSeconds = 10.0;     // exponential time constant
    double delayWindowSeconds = 4.0;  // the delay is searched on the last this many seconds
    double delayEverySeconds = 5.0;
    double maxDelaySeconds = 1.0;
    double delayToleranceMs = 0.5;    // the window's halves must agree with it this closely
    double floorDbfs = -70.0;         // console frames (and search windows) quieter than this don't count
    double smoothing = 6.0;           // 1/N octave
    int pointsPerOctave = 24;
    double coherenceMin = 0.5;
};

struct LiveDelta
{
    std::vector<double> grid;
    std::vector<double> deltaDb;      // mic vs console, 0 dB on the reference band; NaN where nothing was measured
    std::vector<double> coherence;    // 0..1, NaN where nothing was measured
    std::vector<bool> valid;          // coherence at least coherenceMin
    std::optional<double> delayMs;    // the mic after the console; none until found
    int frames = 0;                   // frames averaged since the delay was found
};

class LiveDeltaTracker
{
public:
    explicit LiveDeltaTracker (double fs, std::pair<double, double> refBand = { 250.0, 4000.0 }, const LiveDeltaConfig& cfg = {});

    // Samples of both, taken at the same moments.
    void push (const float* console, const float* mic, std::size_t n);
    void push (const double* console, const double* mic, std::size_t n);

    LiveDelta result() const;
    int framesAveraged() const { return frames; }
    std::optional<std::size_t> delaySamples() const { return delay; }
    double sampleRate() const { return fs; }
    void setReferenceBand (std::pair<double, double> band) { refBand = band; }

private:
    template <typename T>
    void pushSamples (const T* console, const T* mic, std::size_t n);
    std::vector<double> span (const std::vector<double>& buf, std::size_t start, std::size_t length) const;
    void search();
    void frame();

    double fs;
    std::pair<double, double> refBand;
    LiveDeltaConfig cfg;
    std::size_t nfft, hop, windowLen, delayEvery, maxLag, capacity;
    double tolerance, alpha, floor;
    std::vector<double> win, x, y, freqs, grid;
    std::size_t total = 0, nextFrame, nextSearch;
    std::optional<std::size_t> delay;
    int frames = 0;
    std::vector<double> gxx, gyy;
    std::vector<cplx> gxy;
};
} // namespace roomeq
