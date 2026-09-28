#include "roomeq/dualfft.h"

#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;
}

std::size_t estimateDelay (const std::vector<double>& x, const std::vector<double>& y, double fs, double maxDelaySeconds)
{
    const auto n = nextPow2 (x.size() + y.size());
    auto R = rfft (x, n);
    const auto Y = rfft (y, n);
    for (std::size_t k = 0; k < R.size(); ++k)
    {
        R[k] = std::conj (R[k]) * Y[k];
        R[k] /= std::max (std::abs (R[k]), 1e-20);
    }
    const auto r = irfft (R, n);
    const auto maxLag = std::min (static_cast<std::size_t> (maxDelaySeconds * fs), n - 1);
    std::size_t best = 0;
    for (std::size_t lag = 1; lag <= maxLag; ++lag)
        if (r[lag] > r[best])
            best = lag;
    return best;
}

TransferEstimate transferFunction (const std::vector<double>& x, const std::vector<double>& y, double fs,
                                   const DualFFTConfig& cfg, std::size_t nfft)
{
    TransferEstimate est;
    est.fs = fs;
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
