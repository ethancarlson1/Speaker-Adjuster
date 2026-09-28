#include "roomeq/sweep.h"

#include <algorithm>
#include <numeric>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;

// 0 at f <= fa, 1 at f >= fb, raised-cosine in log frequency between.
double raisedCosineLog (double f, double fa, double fb)
{
    auto x = (std::log (std::max (f, 1e-9)) - std::log (fa)) / (std::log (fb) - std::log (fa));
    x = std::clamp (x, 0.0, 1.0);
    return 0.5 * (1.0 - std::cos (pi * x));
}

double median (std::vector<double> v)
{
    const auto n = v.size();
    const auto mid = v.begin() + static_cast<std::ptrdiff_t> (n / 2);
    std::nth_element (v.begin(), mid, v.end());
    if (n % 2 == 1)
        return *mid;
    const auto upper = *mid;
    const auto lower = *std::max_element (v.begin(), mid);
    return 0.5 * (lower + upper);
}
} // namespace

std::vector<double> generateSweep (const SweepConfig& cfg)
{
    const auto n = cfg.nSweep();
    const auto L = cfg.rate();
    std::vector<double> x (n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto t = static_cast<double> (i) / cfg.fs;
        x[i] = std::sin (2.0 * pi * cfg.f1 * L * (std::exp (t / L) - 1.0));
    }

    const auto fadeLength = [&] (double octaves)
    {
        return std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (L * std::log (2.0) * octaves * cfg.fs)));
    };
    const auto nIn = fadeLength (cfg.fadeInOctaves);
    const auto nOut = fadeLength (cfg.fadeOutOctaves);
    for (std::size_t i = 0; i < nIn; ++i)
        x[i] *= 0.5 * (1.0 - std::cos (pi * static_cast<double> (i) / static_cast<double> (nIn)));
    for (std::size_t i = 0; i < nOut; ++i)
        x[n - nOut + i] *= 0.5 * (1.0 + std::cos (pi * static_cast<double> (i) / static_cast<double> (nOut)));

    const auto gain = std::pow (10.0, cfg.levelDbfs / 20.0);
    for (auto& v : x)
        v *= gain;
    return x;
}

std::vector<double> buildPlayback (const SweepConfig& cfg, const std::vector<double>& sweep)
{
    std::vector<double> out (cfg.nRecording(), 0.0);
    std::copy (sweep.begin(), sweep.end(), out.begin() + static_cast<std::ptrdiff_t> (cfg.nPreroll()));
    return out;
}

std::vector<cplx> inverseSpectrum (const std::vector<double>& sweep, std::size_t nfft, double fs,
                                   double f1, double f2, double regInDb, double transitionOctaves)
{
    auto X = rfft (sweep, nfft);
    const auto f = rfftFreqs (nfft, fs);

    std::vector<double> P (X.size());
    std::vector<double> inBand;
    for (std::size_t k = 0; k < X.size(); ++k)
    {
        P[k] = std::norm (X[k]);
        if (f[k] >= f1 && f[k] <= f2)
            inBand.push_back (P[k]);
    }
    const auto epsIn = median (inBand) * std::pow (10.0, regInDb / 10.0);
    const auto epsOut = *std::max_element (inBand.begin(), inBand.end());

    const auto fHiEdge = std::min (f2 * std::pow (2.0, transitionOctaves), fs / 2.0);
    const auto fLoEdge = f1 * std::pow (2.0, -transitionOctaves);

    std::vector<cplx> inv (X.size());
    for (std::size_t k = 0; k < X.size(); ++k)
    {
        auto inside = raisedCosineLog (f[k], fLoEdge, f1);
        if (fHiEdge > f2)
            inside *= 1.0 - raisedCosineLog (f[k], f2, fHiEdge);
        else
            inside *= f[k] <= f2 ? 1.0 : 0.0;
        // Interpolate eps in the log domain so the transition is smooth in dB.
        const auto eps = std::exp (std::log (epsOut) + inside * (std::log (epsIn) - std::log (epsOut)));
        inv[k] = std::conj (X[k]) / (P[k] + eps);
    }
    return inv;
}

Deconvolved deconvolve (const std::vector<double>& recording, const std::vector<double>& sweep,
                        double fs, double f1, double f2)
{
    const auto nRec = recording.size();
    const auto nSw = sweep.size();
    const auto nfft = nextPow2 (nRec + nSw);

    auto H = rfft (recording, nfft);
    const auto inv = inverseSpectrum (sweep, nfft, fs, f1, f2);
    for (std::size_t k = 0; k < H.size(); ++k)
        H[k] *= inv[k];
    const auto h = irfft (H, nfft);

    // Negative times wrapped to the end of the circular result; unwrap them.
    Deconvolved out;
    out.h.reserve (nSw + nRec);
    out.h.insert (out.h.end(), h.end() - static_cast<std::ptrdiff_t> (nSw), h.end());
    out.h.insert (out.h.end(), h.begin(), h.begin() + static_cast<std::ptrdiff_t> (nRec));
    out.zero = nSw;
    out.fs = fs;
    return out;
}

std::size_t findArrival (const Deconvolved& dec, std::size_t preroll, double maxDelaySeconds)
{
    const auto start = dec.zero + preroll;
    const auto stop = std::min (dec.h.size(), start + static_cast<std::size_t> (maxDelaySeconds * dec.fs));
    auto best = start;
    auto bestValue = -1.0;
    for (auto i = start; i < stop; ++i)
    {
        if (std::abs (dec.h[i]) > bestValue)
        {
            bestValue = std::abs (dec.h[i]);
            best = i;
        }
    }
    return best;
}

double fractionalPeakOffset (const std::vector<double>& a, const std::vector<double>& b, int maxLag)
{
    const auto n = nextPow2 (a.size() + b.size());
    auto A = rfft (a, n);
    const auto B = rfft (b, n);
    for (std::size_t k = 0; k < A.size(); ++k)
        A[k] = std::conj (A[k]) * B[k];
    const auto xc = irfft (A, n);

    const auto at = [&] (long lag) { return xc[static_cast<std::size_t> ((lag % static_cast<long> (n) + static_cast<long> (n)) % static_cast<long> (n))]; };

    // Same scan order as the prototype: lags 0..maxLag, then -maxLag..-1.
    long bestLag = 0;
    auto bestValue = at (0);
    for (long lag = 1; lag <= maxLag; ++lag)
        if (at (lag) > bestValue) { bestValue = at (lag); bestLag = lag; }
    for (long lag = -maxLag; lag < 0; ++lag)
        if (at (lag) > bestValue) { bestValue = at (lag); bestLag = lag; }

    const auto y0 = at (bestLag - 1);
    const auto y1 = at (bestLag);
    const auto y2 = at (bestLag + 1);
    const auto denom = y0 - 2.0 * y1 + y2;
    const auto frac = denom != 0.0 ? 0.5 * (y0 - y2) / denom : 0.0;
    return static_cast<double> (bestLag) + std::clamp (frac, -0.5, 0.5);
}
} // namespace roomeq
