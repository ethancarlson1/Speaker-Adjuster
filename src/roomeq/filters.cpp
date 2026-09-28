#include "roomeq/filters.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;
}

const char* bandKindName (BandKind kind)
{
    switch (kind)
    {
        case BandKind::bell:      return "bell";
        case BandKind::lowShelf:  return "low_shelf";
        case BandKind::highShelf: return "high_shelf";
        case BandKind::highPass:  return "high_pass";
        case BandKind::lowPass:   return "low_pass";
    }
    return "bell";
}

bool Band::operator== (const Band& o) const
{
    return kind == o.kind && freq == o.freq && gainDb == o.gainDb && q == o.q && enabled == o.enabled;
}

Band Band::scaled (double amount) const
{
    auto b = *this;
    b.gainDb *= amount;
    return b;
}

double qForBandwidth (double octaves)
{
    return 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * octaves));
}

double bandwidthForQ (double q)
{
    return 2.0 / std::log (2.0) * std::asinh (1.0 / (2.0 * q));
}

Biquad designBiquad (const Band& band, double fs)
{
    if (! band.enabled)
        return {};
    const auto f0 = std::min (band.freq, 0.49 * fs);
    const auto w0 = 2.0 * pi * f0 / fs;
    const auto cw = std::cos (w0);
    const auto sw = std::sin (w0);
    const auto alpha = sw / (2.0 * band.q);
    const auto a = std::pow (10.0, band.gainDb / 40.0);
    double b[3], d[3];
    switch (band.kind)
    {
        case BandKind::bell:
            b[0] = 1.0 + alpha * a; b[1] = -2.0 * cw; b[2] = 1.0 - alpha * a;
            d[0] = 1.0 + alpha / a; d[1] = -2.0 * cw; d[2] = 1.0 - alpha / a;
            break;
        case BandKind::lowShelf:
        {
            const auto k = 2.0 * std::sqrt (a) * alpha;
            b[0] = a * ((a + 1.0) - (a - 1.0) * cw + k);
            b[1] = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
            b[2] = a * ((a + 1.0) - (a - 1.0) * cw - k);
            d[0] = (a + 1.0) + (a - 1.0) * cw + k;
            d[1] = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
            d[2] = (a + 1.0) + (a - 1.0) * cw - k;
            break;
        }
        case BandKind::highShelf:
        {
            const auto k = 2.0 * std::sqrt (a) * alpha;
            b[0] = a * ((a + 1.0) + (a - 1.0) * cw + k);
            b[1] = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
            b[2] = a * ((a + 1.0) + (a - 1.0) * cw - k);
            d[0] = (a + 1.0) - (a - 1.0) * cw + k;
            d[1] = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
            d[2] = (a + 1.0) - (a - 1.0) * cw - k;
            break;
        }
        case BandKind::highPass:
            b[0] = (1.0 + cw) / 2.0; b[1] = -(1.0 + cw); b[2] = (1.0 + cw) / 2.0;
            d[0] = 1.0 + alpha; d[1] = -2.0 * cw; d[2] = 1.0 - alpha;
            break;
        case BandKind::lowPass:
        default:
            b[0] = (1.0 - cw) / 2.0; b[1] = 1.0 - cw; b[2] = (1.0 - cw) / 2.0;
            d[0] = 1.0 + alpha; d[1] = -2.0 * cw; d[2] = 1.0 - alpha;
            break;
    }
    return { b[0] / d[0], b[1] / d[0], b[2] / d[0], d[1] / d[0], d[2] / d[0] };
}

double biquadDb (const Biquad& c, double f, double fs)
{
    const auto w = 2.0 * pi * f / fs;
    const auto z1 = std::polar (1.0, -w);
    const auto z2 = z1 * z1;
    const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10 (std::max (std::abs (num), 1e-300) / std::max (std::abs (den), 1e-300));
}

std::vector<double> bandDb (const Band& band, const std::vector<double>& freqs, double fs)
{
    std::vector<double> out (freqs.size(), 0.0);
    if (! band.enabled)
        return out;
    const auto c = designBiquad (band, fs);
    for (std::size_t i = 0; i < freqs.size(); ++i)
        out[i] = biquadDb (c, freqs[i], fs);
    return out;
}

std::vector<double> responseDb (const std::vector<Band>& bands, const std::vector<double>& freqs, double fs)
{
    std::vector<double> out (freqs.size(), 0.0);
    for (const auto& b : bands)
    {
        const auto r = bandDb (b, freqs, fs);
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] += r[i];
    }
    return out;
}
} // namespace roomeq
