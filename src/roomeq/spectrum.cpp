#include "roomeq/spectrum.h"

#include "roomeq/fft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace roomeq
{
namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

// Cumulative integral of a piecewise-constant spectrum at its bin edges.
struct Cumulative
{
    std::vector<double> edges, values;
};

Cumulative cumulative (const std::vector<double>& power, double df)
{
    Cumulative c;
    c.edges.resize (power.size() + 1);
    c.values.resize (power.size() + 1);
    c.edges[0] = -0.5 * df;
    c.values[0] = 0.0;
    double sum = 0.0;
    for (std::size_t k = 0; k < power.size(); ++k)
    {
        sum += power[k];
        c.edges[k + 1] = (static_cast<double> (k) + 0.5) * df;
        c.values[k + 1] = sum * df;
    }
    return c;
}
} // namespace

Window irWindow (const WindowConfig& cfg, double fs)
{
    const auto nPre = static_cast<std::size_t> (std::llround (cfg.pre * fs));
    const auto nPost = static_cast<std::size_t> (std::llround (cfg.post * fs));
    Window out;
    out.nPre = nPre;
    out.w.assign (nPre + nPost, 1.0);
    const auto nA = std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (static_cast<double> (nPre) * cfg.taperPre)));
    const auto nB = std::max<std::size_t> (1, static_cast<std::size_t> (std::llround (static_cast<double> (nPost) * cfg.taperPost)));
    for (std::size_t i = 0; i < nA; ++i)
        out.w[i] = 0.5 * (1.0 - std::cos (pi * static_cast<double> (i) / static_cast<double> (nA)));
    for (std::size_t i = 0; i < nB; ++i)
        out.w[out.w.size() - nB + i] = 0.5 * (1.0 + std::cos (pi * static_cast<double> (i) / static_cast<double> (nB)));
    return out;
}

std::size_t responseNfft (const WindowConfig& cfg, double fs)
{
    const auto nPre = static_cast<std::size_t> (std::llround (cfg.pre * fs));
    const auto nPost = static_cast<std::size_t> (std::llround (cfg.post * fs));
    return nextPow2 (nPre + nPost);
}

std::vector<double> logFreqGrid (double fLo, double fHi, int pointsPerOctave)
{
    const auto n = static_cast<std::size_t> (std::floor (std::log2 (fHi / fLo) * pointsPerOctave)) + 1;
    std::vector<double> g (n);
    for (std::size_t i = 0; i < n; ++i)
        g[i] = fLo * std::pow (2.0, static_cast<double> (i) / pointsPerOctave);
    return g;
}

double interp (double x, const std::vector<double>& xp, const std::vector<double>& fp)
{
    if (x <= xp.front())
        return fp.front();
    if (x >= xp.back())
        return fp.back();
    const auto it = std::upper_bound (xp.begin(), xp.end(), x);
    const auto j = static_cast<std::size_t> (it - xp.begin());
    const auto x0 = xp[j - 1], x1 = xp[j];
    const auto t = (x - x0) / (x1 - x0);
    return fp[j - 1] + t * (fp[j] - fp[j - 1]);
}

std::vector<double> smoothPower (const std::vector<double>& freqs, const std::vector<double>& power,
                                 double fraction, const std::vector<double>& outFreqs,
                                 const std::vector<double>* weights)
{
    const auto df = freqs[1] - freqs[0];
    const auto weightAt = [&] (std::size_t k) { return weights != nullptr ? (*weights)[k] : 1.0; };
    std::vector<double> out (outFreqs.size());

    if (fraction <= 0.0)
    {
        for (std::size_t i = 0; i < outFreqs.size(); ++i)
        {
            const auto idx = static_cast<std::size_t> (std::clamp (std::nearbyint (outFreqs[i] / df), 0.0,
                                                                   static_cast<double> (power.size() - 1)));
            out[i] = weightAt (idx) > 0.0 ? power[idx] : nan;
        }
        return out;
    }

    std::vector<double> wp (power.size()), w (power.size());
    for (std::size_t k = 0; k < power.size(); ++k)
    {
        w[k] = weightAt (k);
        wp[k] = w[k] > 0.0 ? w[k] * power[k] : 0.0;
    }
    const auto cwp = cumulative (wp, df);
    const auto cw = cumulative (w, df);

    const auto half = std::pow (2.0, 1.0 / (2.0 * fraction));
    for (std::size_t i = 0; i < outFreqs.size(); ++i)
    {
        const auto lo = outFreqs[i] / half;
        const auto hi = outFreqs[i] * half;
        const auto num = interp (hi, cwp.edges, cwp.values) - interp (lo, cwp.edges, cwp.values);
        const auto den = interp (hi, cw.edges, cw.values) - interp (lo, cw.edges, cw.values);
        out[i] = den > 1e-12 * df ? num / den : nan;
    }
    return out;
}

std::vector<double> rebinPower (const std::vector<double>& freqs, const std::vector<double>& power,
                                const std::vector<double>& outFreqs)
{
    const auto dfIn = freqs[1] - freqs[0];
    const auto dfOut = outFreqs[1] - outFreqs[0];
    const auto c = cumulative (power, dfIn);
    std::vector<double> out (outFreqs.size());
    for (std::size_t i = 0; i < outFreqs.size(); ++i)
    {
        const auto lo = std::max (outFreqs[i] - 0.5 * dfOut, c.edges.front());
        const auto hi = std::min (outFreqs[i] + 0.5 * dfOut, c.edges.back());
        out[i] = (interp (hi, c.edges, c.values) - interp (lo, c.edges, c.values)) / std::max (hi - lo, 1e-12);
    }
    return out;
}

double toDb (double power, double floor)
{
    if (std::isnan (power))
        return nan;
    return 10.0 * std::log10 (std::max (power, floor));
}

std::vector<double> toDb (const std::vector<double>& power, double floor)
{
    std::vector<double> out (power.size());
    for (std::size_t i = 0; i < power.size(); ++i)
        out[i] = toDb (power[i], floor);
    return out;
}

std::pair<double, double> octaveEdges (double center)
{
    return { center / std::sqrt (2.0), center * std::sqrt (2.0) };
}

std::string formatHz (double f)
{
    if (! (f > 0.0))
        return "0 Hz";
    // Round to two significant figures, like Python's f"{f:.2g}".
    const auto exponent = std::floor (std::log10 (f));
    const auto scale = std::pow (10.0, exponent - 1.0);
    const auto rounded = std::nearbyint (f / scale) * scale;
    char buf[32];
    if (rounded >= 1000.0)
        std::snprintf (buf, sizeof (buf), "%g kHz", rounded / 1000.0);
    else
        std::snprintf (buf, sizeof (buf), "%g Hz", rounded);
    return buf;
}

BandSum bandSum (const std::vector<double>& freqs, const std::vector<double>& values, double lo, double hi)
{
    BandSum s;
    for (std::size_t k = 0; k < freqs.size(); ++k)
    {
        if (freqs[k] >= lo && freqs[k] < hi)
        {
            s.sum += values[k];
            ++s.count;
        }
    }
    return s;
}

double nanMean (const std::vector<double>& v)
{
    double sum = 0.0;
    int n = 0;
    for (auto x : v)
    {
        if (! std::isnan (x))
        {
            sum += x;
            ++n;
        }
    }
    return n > 0 ? sum / n : nan;
}
} // namespace roomeq
