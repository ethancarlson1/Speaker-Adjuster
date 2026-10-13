#include "roomeq/livedelta.h"

#include "roomeq/dualfft.h"
#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;

double rms (const std::vector<double>& v)
{
    double sum = 0.0;
    for (auto s : v)
        sum += s * s;
    return v.empty() ? 0.0 : std::sqrt (sum / static_cast<double> (v.size()));
}

bool distant (std::size_t a, std::size_t b, double tolerance)
{
    return std::abs (static_cast<double> (a) - static_cast<double> (b)) > tolerance;
}
} // namespace

LiveDeltaTracker::LiveDeltaTracker (double sampleRate, std::pair<double, double> band, const LiveDeltaConfig& config)
    : fs (sampleRate), refBand (band), cfg (config)
{
    nfft = nextPow2 (static_cast<std::size_t> (cfg.fftSeconds * fs));
    hop = std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (cfg.hopSeconds * fs)));
    windowLen = static_cast<std::size_t> (std::llround (cfg.delayWindowSeconds * fs));
    delayEvery = std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (cfg.delayEverySeconds * fs)));
    maxLag = static_cast<std::size_t> (cfg.maxDelaySeconds * fs);
    tolerance = cfg.delayToleranceMs * 1e-3 * fs;
    alpha = 1.0 - std::exp (-(static_cast<double> (hop) / fs) / cfg.averageSeconds);
    floor = std::pow (10.0, cfg.floorDbfs / 20.0);
    win.resize (nfft);
    for (std::size_t i = 0; i < nfft; ++i)   // numpy.hanning (symmetric)
        win[i] = 0.5 - 0.5 * std::cos (2.0 * pi * static_cast<double> (i) / static_cast<double> (nfft - 1));
    capacity = nextPow2 (hop + std::max (windowLen, nfft + maxLag));
    x.assign (capacity, 0.0);
    y.assign (capacity, 0.0);
    nextFrame = hop;
    nextSearch = delayEvery;
    const auto bins = nfft / 2 + 1;
    gxx.assign (bins, 0.0);
    gyy.assign (bins, 0.0);
    gxy.assign (bins, cplx {});
    freqs = rfftFreqs (nfft, fs);
    grid = logFreqGrid (20.0, std::min (20000.0, 0.45 * fs), cfg.pointsPerOctave);
}

void LiveDeltaTracker::push (const float* console, const float* mic, std::size_t n) { pushSamples (console, mic, n); }
void LiveDeltaTracker::push (const double* console, const double* mic, std::size_t n) { pushSamples (console, mic, n); }

template <typename T>
void LiveDeltaTracker::pushSamples (const T* console, const T* mic, std::size_t n)
{
    std::size_t pos = 0;
    while (pos < n)
    {
        const auto due = std::min (nextFrame, nextSearch);
        const auto take = std::min (n - pos, due - total);
        for (std::size_t i = 0; i < take; ++i)
        {
            const auto at = (total + i) % capacity;
            x[at] = static_cast<double> (console[pos + i]);
            y[at] = static_cast<double> (mic[pos + i]);
        }
        total += take;
        pos += take;
        if (total == nextSearch)
        {
            search();
            nextSearch += delayEvery;
        }
        if (total == nextFrame)
        {
            frame();
            nextFrame += hop;
        }
    }
}

std::vector<double> LiveDeltaTracker::span (const std::vector<double>& buf, std::size_t start, std::size_t length) const
{
    std::vector<double> out (length);
    for (std::size_t i = 0; i < length; ++i)
        out[i] = buf[(start + i) % capacity];
    return out;
}

void LiveDeltaTracker::search()
{
    if (total < windowLen)
        return;
    const auto start = total - windowLen;
    const auto xs = span (x, start, windowLen), ys = span (y, start, windowLen);
    if (rms (xs) < floor || std::all_of (ys.begin(), ys.end(), [] (double v) { return v == 0.0; }))
        return;
    const auto half = windowLen / 2;
    const std::vector<double> x1 (xs.begin(), xs.begin() + static_cast<std::ptrdiff_t> (half)),
        y1 (ys.begin(), ys.begin() + static_cast<std::ptrdiff_t> (half)), x2 (xs.begin() + static_cast<std::ptrdiff_t> (half), xs.end()),
        y2 (ys.begin() + static_cast<std::ptrdiff_t> (half), ys.end());
    const auto d = estimateDelay (xs, ys, fs, cfg.maxDelaySeconds);
    const auto d1 = estimateDelay (x1, y1, fs, cfg.maxDelaySeconds);
    const auto d2 = estimateDelay (x2, y2, fs, cfg.maxDelaySeconds);
    if (distant (d1, d, tolerance) || distant (d2, d, tolerance))
        return;
    if (! delay || distant (d, *delay, tolerance))
    {
        delay = d;
        frames = 0;
        std::fill (gxx.begin(), gxx.end(), 0.0);
        std::fill (gyy.begin(), gyy.end(), 0.0);
        std::fill (gxy.begin(), gxy.end(), cplx {});
    }
}

void LiveDeltaTracker::frame()
{
    if (! delay || total < *delay + nfft)
        return;
    auto xf = span (x, total - *delay - nfft, nfft);
    if (rms (xf) < floor)
        return;
    auto yf = span (y, total - nfft, nfft);
    for (std::size_t i = 0; i < nfft; ++i)
    {
        xf[i] *= win[i];
        yf[i] *= win[i];
    }
    const auto X = rfft (xf, nfft), Y = rfft (yf, nfft);
    for (std::size_t k = 0; k < X.size(); ++k)
    {
        const auto pxx = std::norm (X[k]), pyy = std::norm (Y[k]);
        const auto pxy = std::conj (X[k]) * Y[k];
        if (frames == 0)
        {
            gxx[k] = pxx;
            gyy[k] = pyy;
            gxy[k] = pxy;
        }
        else
        {
            gxx[k] += alpha * (pxx - gxx[k]);
            gyy[k] += alpha * (pyy - gyy[k]);
            gxy[k] += alpha * (pxy - gxy[k]);
        }
    }
    ++frames;
}

LiveDelta LiveDeltaTracker::result() const
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto n = grid.size();
    LiveDelta r;
    r.grid = grid;
    r.deltaDb.assign (n, nan);
    r.coherence.assign (n, nan);
    r.valid.assign (n, false);
    if (delay)
        r.delayMs = 1000.0 * static_cast<double> (*delay) / fs;
    r.frames = frames;
    if (frames == 0)
        return r;

    std::vector<double> h2 (gxx.size()), coh (gxx.size());
    for (std::size_t k = 0; k < gxx.size(); ++k)
    {
        const auto cross = std::norm (gxy[k]);
        h2[k] = gxx[k] > 0.0 ? cross / (gxx[k] * gxx[k]) : 0.0;
        coh[k] = gxx[k] > 0.0 && gyy[k] > 0.0 ? std::clamp (cross / (gxx[k] * gyy[k]), 0.0, 1.0) : 0.0;
    }
    const auto power = smoothPower (freqs, h2, cfg.smoothing, grid, &gxx);
    const auto coherence = smoothPower (freqs, coh, cfg.smoothing, grid, &gxx);
    double bandSum = 0.0, allSum = 0.0;
    int bandCount = 0, allCount = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (! (std::isfinite (power[i]) && power[i] > 0.0))
            continue;
        r.deltaDb[i] = toDb (power[i]);
        r.coherence[i] = coherence[i];
        r.valid[i] = coherence[i] >= cfg.coherenceMin;
        if (r.valid[i])
        {
            allSum += r.deltaDb[i];
            ++allCount;
            if (grid[i] >= refBand.first && grid[i] <= refBand.second)
            {
                bandSum += r.deltaDb[i];
                ++bandCount;
            }
        }
    }
    if (allCount == 0)
    {
        r.deltaDb.assign (n, nan);
        return r;
    }
    const auto offset = bandCount > 0 ? bandSum / bandCount : allSum / allCount;
    for (auto& v : r.deltaDb)
        v -= offset;
    return r;
}
} // namespace roomeq
