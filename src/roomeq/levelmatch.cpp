#include "roomeq/levelmatch.h"

#include <algorithm>
#include <cmath>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;

// |H(e^jw)|^2 of a normalised biquad, from cos(w) and cos(2w).
double powerGain (const Biquad& c, double c1, double c2) noexcept
{
    const auto num = c.b0 * c.b0 + c.b1 * c.b1 + c.b2 * c.b2 + 2.0 * (c.b0 * c.b1 + c.b1 * c.b2) * c1 + 2.0 * c.b0 * c.b2 * c2;
    const auto den = 1.0 + c.a1 * c.a1 + c.a2 * c.a2 + 2.0 * (c.a1 + c.a1 * c.a2) * c1 + 2.0 * c.a2 * c2;
    return den > 0.0 ? std::max (num, 0.0) / den : 1.0;
}
} // namespace

std::array<Band, 2> kWeightingBands()
{
    return { Band { BandKind::highShelf, 1681.974450955533, 3.999843853973347, 0.7071752369554196 },
             Band { BandKind::highPass, 38.13547087602444, 0.0, 0.5003270373238773 } };
}

void LevelMatch::prepare (double fs) noexcept
{
    sampleRate = fs;
    const auto fLo = 20.0;
    const auto fHi = std::min (20000.0, 0.45 * fs);
    const auto octaves = std::log2 (fHi / fLo);
    count = std::clamp (static_cast<int> (std::ceil (octaves * pointsPerOctave)) + 1, 2, maxPoints);
    const auto k = kWeightingBands();
    const auto k0 = designBiquad (k[0], fs);
    const auto k1 = designBiquad (k[1], fs);
    weightSum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        // Log-spaced points each stand for the same fraction of an octave: equal pink-noise power.
        const auto f = fLo * std::exp2 (octaves * i / (count - 1));
        const auto w = 2.0 * pi * f / fs;
        const auto n = static_cast<std::size_t> (i);
        cos1[n] = std::cos (w);
        cos2[n] = std::cos (2.0 * w);
        weight[n] = powerGain (k0, cos1[n], cos2[n]) * powerGain (k1, cos1[n], cos2[n]);
        weightSum += weight[n];
    }
}

void LevelMatch::multiply (Power& p, const Band& band) const noexcept
{
    if (! band.enabled)
        return;
    const auto c = designBiquad (band, sampleRate);
    for (int i = 0; i < count; ++i)
    {
        const auto n = static_cast<std::size_t> (i);
        p[n] *= powerGain (c, cos1[n], cos2[n]);
    }
}

double LevelMatch::makeupDb (const Power& a, const Power& b) const noexcept
{
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto n = static_cast<std::size_t> (i);
        sum += weight[n] * a[n] * b[n];
    }
    const auto db = -10.0 * std::log10 (sum / weightSum);
    return std::isfinite (db) ? std::clamp (db, -maxGainDb, maxGainDb) : 0.0;
}

double LevelMatch::makeupDb (const std::vector<Band>& bands) const
{
    Power p {}, one {};
    flat (p);
    flat (one);
    for (const auto& b : bands)
        multiply (p, b);
    return makeupDb (p, one);
}
} // namespace roomeq
