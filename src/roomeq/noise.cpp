#include "roomeq/noise.h"

#include "roomeq/fft.h"

#include <algorithm>
#include <random>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;
}

std::vector<double> generatePinkNoise (const NoiseConfig& cfg)
{
    const auto n = cfg.nNoise();
    const auto nfft = nextPow2 (n);

    std::mt19937 rng (cfg.seed);
    std::normal_distribution<double> dist (0.0, 1.0);
    std::vector<double> white (nfft);
    for (auto& v : white)
        v = dist (rng);

    auto spec = rfft (white, nfft);
    const auto f = rfftFreqs (nfft, cfg.fs);
    for (std::size_t k = 0; k < spec.size(); ++k)
        spec[k] *= f[k] >= cfg.f1 && f[k] <= cfg.f2 ? 1.0 / std::sqrt (std::max (f[k], cfg.f1)) : 0.0;
    auto x = irfft (spec, nfft);
    x.resize (n);

    // Clip the rare peaks at 3 sigma: ~4.5 dB more signal for the same peak
    // level. The analysis compares against exactly what was played.
    double sumSq = 0.0;
    for (auto v : x)
        sumSq += v * v;
    const auto rms = std::sqrt (sumSq / static_cast<double> (n));
    for (auto& v : x)
        v = std::clamp (v / rms, -3.0, 3.0);

    const auto nFade = std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (cfg.fade * cfg.fs)));
    for (std::size_t i = 0; i < nFade && i < n; ++i)
    {
        const auto ramp = 0.5 * (1.0 - std::cos (pi * static_cast<double> (i) / static_cast<double> (nFade)));
        x[i] *= ramp;
        x[n - 1 - i] *= ramp;
    }

    double peak = 0.0;
    for (auto v : x)
        peak = std::max (peak, std::abs (v));
    const auto gain = std::pow (10.0, cfg.levelDbfs / 20.0) / std::max (peak, 1e-30);
    for (auto& v : x)
        v *= gain;
    return x;
}
} // namespace roomeq
