#include "roomeq/dualfft.h"

#include "roomeq/spectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;

// PHAT-weighted cross spectrum of x and y, zero-padded to a power of two.
std::vector<cplx> phatSpectrum (const std::vector<double>& x, const std::vector<double>& y, std::size_t n)
{
    auto R = rfft (x, n);
    const auto Y = rfft (y, n);
    for (std::size_t k = 0; k < R.size(); ++k)
    {
        R[k] = std::conj (R[k]) * Y[k];
        R[k] /= std::max (std::abs (R[k]), 1e-20);
    }
    return R;
}

// Sub-sample peak of the band-limited correlation irfft(R) near integer lag k:
// exact between samples (a parabola through three samples is biased, and the
// bias moves as a drifting delay slides). Newton steps on r'(tau) = 0.
double refinePeak (const std::vector<cplx>& R, std::size_t n, std::size_t k)
{
    const auto count = R.size();
    auto tau = static_cast<double> (k);
    const auto lo = tau - 1.0, hi = tau + 1.0;
    for (int it = 0; it < 6; ++it)
    {
        double d1 = 0.0, d2 = 0.0;
        const auto dw = 2.0 * pi / static_cast<double> (n);
        const auto z = std::polar (1.0, dw * tau);
        cplx e { 1.0, 0.0 };                       // e^(j w tau), stepped by z (re-anchored now and then)
        for (std::size_t b = 0; b < count; ++b, e *= z)
        {
            if (b % 4096 == 0)
                e = std::polar (1.0, dw * static_cast<double> (b) * tau);
            const auto w = dw * static_cast<double> (b);
            const auto scale = b == 0 || b == count - 1 ? 1.0 : 2.0;   // DC and Nyquist appear once
            const auto a = scale * R[b] * e;
            d1 += std::real (cplx (0.0, w) * a);
            d2 += std::real (-(w * w) * a);
        }
        if (d2 >= 0.0)
            break;
        const auto step = -d1 / d2;
        tau = std::clamp (tau + step, lo, hi);
        if (std::abs (step) < 1e-6)
            break;
    }
    return tau;
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    const auto m = v.size() / 2;
    return v.size() % 2 == 1 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

// Windowed-sinc interpolation: 64 taps, Kaiser (beta 8), 512 fractional phases.
constexpr int taps = 64, phases = 512;
constexpr double beta = 8.0;

double besselI0 (double v)
{
    double term = 1.0, total = 1.0;
    for (int m = 1; term > 1e-17 * total; ++m)
    {
        term *= (v / (2.0 * m)) * (v / (2.0 * m));
        total += term;
    }
    return total;
}

const std::vector<std::array<double, taps>>& sincTable()
{
    static const auto table = []
    {
        constexpr int half = taps / 2;
        std::vector<std::array<double, taps>> t (phases + 1);
        const auto norm = besselI0 (beta);
        for (int p = 0; p <= phases; ++p)
        {
            double sum = 0.0;
            auto& row = t[static_cast<std::size_t> (p)];
            for (int j = 0; j < taps; ++j)
            {
                const auto u = static_cast<double> (j - half + 1) - static_cast<double> (p) / phases;
                const auto r = u / half;
                const auto w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / norm;
                const auto sinc = u == 0.0 ? 1.0 : std::sin (pi * u) / (pi * u);
                row[static_cast<std::size_t> (j)] = sinc * w;
                sum += sinc * w;
            }
            for (auto& v : row)
                v /= sum;
        }
        return t;
    }();
    return table;
}
} // namespace

double interpolateAt (const std::vector<double>& y, double position)
{
    constexpr int half = taps / 2;
    const auto& table = sincTable();
    const auto base = std::floor (position);
    const auto ph = (position - base) * phases;
    const auto p = std::min (static_cast<int> (std::floor (ph)), phases - 1);
    const auto a = ph - p;
    const auto& t0 = table[static_cast<std::size_t> (p)];
    const auto& t1 = table[static_cast<std::size_t> (p + 1)];
    const auto first = static_cast<long long> (base) - half + 1;
    double acc = 0.0;
    for (int j = 0; j < taps; ++j)
    {
        const auto i = first + j;
        if (i < 0 || i >= static_cast<long long> (y.size()))
            continue;
        const auto c = (1.0 - a) * t0[static_cast<std::size_t> (j)] + a * t1[static_cast<std::size_t> (j)];
        acc += y[static_cast<std::size_t> (i)] * c;
    }
    return acc;
}

std::vector<double> removeDrift (const std::vector<double>& y, double ppm)
{
    const auto rate = 1.0 + ppm * 1e-6;
    std::vector<double> out (y.size());
    for (std::size_t n = 0; n < y.size(); ++n)
        out[n] = interpolateAt (y, static_cast<double> (n) * rate);
    return out;
}

namespace
{
struct BlockDelays
{
    std::vector<double> t, d;   // block centre (samples) and delay (sub-sample), clear peaks only
};

BlockDelays blockDelays (const std::vector<double>& x, const std::vector<double>& y, double fs, const DualFFTConfig& cfg)
{
    const auto block = static_cast<std::size_t> (std::llround (cfg.driftBlockSeconds * fs));
    const auto hop = block / 2;
    const auto maxLag = static_cast<std::size_t> (cfg.maxDelaySeconds * fs);
    std::vector<double> times, delays, peaks;
    for (std::size_t s = 0; hop > 0 && s + block <= x.size() && s + block + maxLag <= y.size(); s += hop)
    {
        const std::vector<double> xb (x.begin() + static_cast<long> (s), x.begin() + static_cast<long> (s + block));
        const std::vector<double> yb (y.begin() + static_cast<long> (s), y.begin() + static_cast<long> (s + block + maxLag));
        const auto n = nextPow2 (xb.size() + yb.size());
        const auto R = phatSpectrum (xb, yb, n);
        const auto r = irfft (R, n);
        std::size_t k = 0;
        for (std::size_t lag = 1; lag <= std::min (maxLag, n - 1); ++lag)
            if (r[lag] > r[k])
                k = lag;
        times.push_back (static_cast<double> (s) + static_cast<double> (block) / 2.0);
        delays.push_back (refinePeak (R, n, k));
        peaks.push_back (r[k]);
    }
    BlockDelays out;
    if (peaks.empty())
        return out;
    const auto strongest = *std::max_element (peaks.begin(), peaks.end());
    for (std::size_t i = 0; i < peaks.size(); ++i)
        if (peaks[i] >= 0.3 * strongest)
        {
            out.t.push_back (times[i]);
            out.d.push_back (delays[i]);
        }
    return out;
}

// Theil-Sen slope (samples of delay per sample) and how many blocks sit on the line.
std::pair<double, int> fitLine (const BlockDelays& b, const DualFFTConfig& cfg)
{
    std::vector<double> slopes;
    for (std::size_t i = 0; i < b.t.size(); ++i)
        for (std::size_t j = i + 1; j < b.t.size(); ++j)
            slopes.push_back ((b.d[j] - b.d[i]) / (b.t[j] - b.t[i]));
    const auto slope = median (slopes);
    std::vector<double> offsets;
    for (std::size_t i = 0; i < b.t.size(); ++i)
        offsets.push_back (b.d[i] - slope * b.t[i]);
    const auto offset = median (offsets);
    int inliers = 0;
    for (std::size_t i = 0; i < b.t.size(); ++i)
        if (std::abs (b.d[i] - (offset + slope * b.t[i])) <= cfg.driftTolerance)
            ++inliers;
    return { slope, inliers };
}
} // namespace

DriftEstimate estimateDrift (const std::vector<double>& x, const std::vector<double>& y, double fs, const DualFFTConfig& cfg)
{
    // A fast drift smears each block's peak (40 ppm slides 6 samples in 3 s), so
    // when there is one, what's left after taking the first estimate out is
    // measured again, on sharp peaks, and added to it.
    DriftEstimate d;
    d.measuredPpm = std::numeric_limits<double>::quiet_NaN();
    auto blocks = blockDelays (x, y, fs, cfg);
    d.blocks = static_cast<int> (blocks.t.size());
    if (blocks.t.size() < 3)
        return d;
    auto [slope, inliers] = fitLine (blocks, cfg);
    auto ppm = slope * 1e6;
    if (std::abs (ppm) >= cfg.driftMinPpm && std::abs (ppm) <= cfg.driftMaxPpm)
    {
        auto second = blockDelays (x, removeDrift (y, ppm), fs, cfg);
        if (second.t.size() >= 3)
        {
            const auto [slope2, inliers2] = fitLine (second, cfg);
            ppm = ((1.0 + ppm * 1e-6) * (1.0 + slope2) - 1.0) * 1e6;
            inliers = inliers2;
            blocks = std::move (second);
            d.blocks = static_cast<int> (blocks.t.size());
        }
    }
    d.measuredPpm = ppm;
    using Status = DriftEstimate::Status;
    const auto count = static_cast<double> (blocks.t.size());
    if ((blocks.t.size() >= 4 && inliers < 0.6 * count) || std::abs (ppm) > cfg.driftMaxPpm)
        d.status = Status::unsteady;
    else if (std::abs (ppm) < cfg.driftMinPpm)
        d.status = Status::none;
    else
    {
        d.status = Status::corrected;
        d.ppm = ppm;
    }
    return d;
}

std::vector<std::string> driftNotes (const DriftEstimate& drift)
{
    if (drift.status == DriftEstimate::Status::corrected)
    {
        char text[96];
        std::snprintf (text, sizeof (text), "output and mic clocks differ by %.1f ppm (corrected)", std::abs (drift.ppm));
        return { text };
    }
    if (drift.status == DriftEstimate::Status::unsteady)
        return { "the delay between the output and the mic jumps around: use one audio interface for both" };
    return {};
}

std::size_t estimateDelay (const std::vector<double>& x, const std::vector<double>& y, double fs, double maxDelaySeconds)
{
    const auto n = nextPow2 (x.size() + y.size());
    const auto R = phatSpectrum (x, y, n);
    const auto r = irfft (R, n);
    const auto maxLag = std::min (static_cast<std::size_t> (maxDelaySeconds * fs), n - 1);
    std::size_t best = 0;
    for (std::size_t lag = 1; lag <= maxLag; ++lag)
        if (r[lag] > r[best])
            best = lag;
    return best;
}

TransferEstimate transferFunction (const std::vector<double>& x, const std::vector<double>& yIn, double fs,
                                   const DualFFTConfig& cfg, std::size_t nfft)
{
    TransferEstimate est;
    est.fs = fs;
    est.drift = estimateDrift (x, yIn, fs, cfg);
    const auto corrected = est.drift.status == DriftEstimate::Status::corrected;
    const auto resampled = corrected ? removeDrift (yIn, est.drift.ppm) : std::vector<double> {};
    const auto& y = corrected ? resampled : yIn;
    est.delay = estimateDelay (x, y, fs, cfg.maxDelaySeconds);

    const auto yLen = y.size() > est.delay ? y.size() - est.delay : 0;
    const auto n = std::min (x.size(), yLen);

    est.nfft = nfft != 0 ? nfft : nextPow2 (static_cast<std::size_t> (cfg.fftSeconds * fs));
    const auto N = est.nfft;
    const auto hop = std::max<std::size_t> (1, static_cast<std::size_t> (static_cast<double> (N) * (1.0 - cfg.overlap)));

    std::vector<double> win (N);
    for (std::size_t i = 0; i < N; ++i)   // numpy.hanning (symmetric)
        win[i] = 0.5 - 0.5 * std::cos (2.0 * pi * static_cast<double> (i) / static_cast<double> (N - 1));

    const auto bins = N / 2 + 1;
    est.gxx.assign (bins, 0.0);
    est.gyy.assign (bins, 0.0);
    std::vector<cplx> gxy (bins, cplx {});
    std::vector<double> xs (N), ys (N);
    for (std::size_t s = 0; n >= N && s <= n - N; s += hop)
    {
        for (std::size_t i = 0; i < N; ++i)
        {
            xs[i] = win[i] * x[s + i];
            ys[i] = win[i] * y[est.delay + s + i];
        }
        const auto X = rfft (xs, N);
        const auto Y = rfft (ys, N);
        for (std::size_t k = 0; k < bins; ++k)
        {
            est.gxx[k] += std::norm (X[k]);
            est.gyy[k] += std::norm (Y[k]);
            gxy[k] += std::conj (X[k]) * Y[k];
        }
        ++est.segments;
    }
    if (est.segments == 0)
        throw std::runtime_error ("program excerpt shorter than one FFT frame");

    const auto k = static_cast<double> (est.segments);
    est.H.resize (bins);
    est.coherence.resize (bins);
    for (std::size_t b = 0; b < bins; ++b)
    {
        est.gxx[b] /= k;
        est.gyy[b] /= k;
        gxy[b] /= k;
        est.H[b] = est.gxx[b] > 0.0 ? gxy[b] / est.gxx[b] : cplx {};
        const auto coh = est.gxx[b] > 0.0 && est.gyy[b] > 0.0 ? std::norm (gxy[b]) / (est.gxx[b] * est.gyy[b]) : 0.0;
        est.coherence[b] = std::clamp (coh, 0.0, 1.0);
    }
    est.freqs = rfftFreqs (N, fs);
    est.effectiveAverages = static_cast<double> (n) / static_cast<double> (N);
    return est;
}

std::vector<double> noisePowerHDomain (const TransferEstimate& est)
{
    std::vector<double> out (est.gxx.size());
    for (std::size_t k = 0; k < out.size(); ++k)
    {
        const auto nop = (1.0 - est.coherence[k]) * est.gyy[k];
        out[k] = est.gxx[k] > 0.0 ? nop / (est.gxx[k] * est.effectiveAverages)
                                  : std::numeric_limits<double>::infinity();
    }
    return out;
}

std::vector<double> excitationGate (const TransferEstimate& est, double floorDb)
{
    auto at = est.freqs;
    at[0] = est.freqs[1];
    const auto local = smoothPower (est.freqs, est.gxx, 3.0, at);
    std::vector<double> gate (est.gxx.size());
    const auto floor = std::pow (10.0, floorDb / 10.0);
    for (std::size_t k = 0; k < gate.size(); ++k)
        gate[k] = est.gxx[k] > local[k] * floor ? 1.0 : 0.0;
    return gate;
}
} // namespace roomeq
