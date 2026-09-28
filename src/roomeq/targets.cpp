#include "roomeq/targets.h"

#include <algorithm>
#include <cmath>

namespace roomeq
{
bool TargetCurve::operator== (const TargetCurve& o) const
{
    return name == o.name && points == o.points;
}

std::vector<double> TargetCurve::db (const std::vector<double>& freqs) const
{
    return pchipLog (points, freqs);
}

double TargetCurve::db (double freq) const
{
    return pchipLog (points, { freq }).front();
}

const TargetCurve& flatTarget()
{
    static const TargetCurve t { "Flat", { { 1000.0, 0.0 } } };
    return t;
}

// +4 dB below ~80 Hz easing to 0 by 250 Hz; flat mids; ~-1 dB/octave above 2 kHz (-3 dB at 16 kHz).
const TargetCurve& houseTarget()
{
    static const TargetCurve t { "House", {
        { 20.0, 4.0 }, { 50.0, 4.0 }, { 80.0, 3.4 }, { 125.0, 1.9 }, { 180.0, 0.6 }, { 250.0, 0.0 },
        { 2000.0, 0.0 }, { 4000.0, -1.0 }, { 8000.0, -2.0 }, { 16000.0, -3.0 }, { 20000.0, -3.3 } } };
    return t;
}

// Rolled off below 150 Hz (-6 dB at 75 Hz), +2 dB presence at 2-4 kHz, -3 dB at 16 kHz.
const TargetCurve& speechTarget()
{
    static const TargetCurve t { "Speech", {
        { 20.0, -12.0 }, { 35.0, -11.0 }, { 50.0, -9.0 }, { 75.0, -6.0 }, { 100.0, -3.8 }, { 150.0, -1.2 },
        { 250.0, 0.0 }, { 1200.0, 0.0 }, { 2000.0, 1.6 }, { 2800.0, 2.0 }, { 4000.0, 1.6 }, { 6000.0, 0.0 },
        { 8000.0, -1.0 }, { 16000.0, -3.0 }, { 20000.0, -3.3 } } };
    return t;
}

const std::vector<TargetCurve>& targetPresets()
{
    static const std::vector<TargetCurve> presets { flatTarget(), houseTarget(), speechTarget() };
    return presets;
}

namespace
{
double sign (double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }
} // namespace

std::vector<double> pchipLog (const std::vector<std::pair<double, double>>& points, const std::vector<double>& freqs)
{
    std::vector<double> out (freqs.size(), 0.0);
    if (points.empty())
        return out;
    if (points.size() == 1)
    {
        std::fill (out.begin(), out.end(), points.front().second);
        return out;
    }
    const auto n = points.size();
    std::vector<double> x (n), y (n), h (n - 1), delta (n - 1), m (n, 0.0);
    for (std::size_t k = 0; k < n; ++k)
    {
        x[k] = std::log2 (points[k].first);
        y[k] = points[k].second;
    }
    for (std::size_t k = 0; k + 1 < n; ++k)
    {
        h[k] = x[k + 1] - x[k];
        delta[k] = (y[k + 1] - y[k]) / h[k];
    }
    m[0] = delta[0];
    m[n - 1] = delta[n - 2];
    for (std::size_t k = 1; k + 1 < n; ++k)
    {
        if (delta[k - 1] * delta[k] > 0.0)
        {
            const auto w1 = 2.0 * h[k] + h[k - 1];
            const auto w2 = h[k] + 2.0 * h[k - 1];
            m[k] = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k]);
        }
    }
    if (n > 2)   // keep the end slopes from overshooting (scipy's pchip end rule)
    {
        const auto endSlope = [] (double d0, double d1, double h0, double h1)
        {
            auto s = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
            if (sign (s) != sign (d0))
                s = 0.0;
            else if (sign (d0) != sign (d1) && std::abs (s) > std::abs (3.0 * d0))
                s = 3.0 * d0;
            return s;
        };
        m[0] = endSlope (delta[0], delta[1], h[0], h[1]);
        m[n - 1] = endSlope (delta[n - 2], delta[n - 3], h[n - 2], h[n - 3]);
    }

    for (std::size_t q = 0; q < freqs.size(); ++q)
    {
        const auto xq = std::clamp (std::log2 (std::max (freqs[q], 1e-3)), x.front(), x.back());
        auto i = static_cast<std::size_t> (std::upper_bound (x.begin(), x.end(), xq) - x.begin());
        i = std::clamp<std::size_t> (i == 0 ? 0 : i - 1, 0, n - 2);
        const auto t = (xq - x[i]) / h[i];
        const auto h00 = (1.0 + 2.0 * t) * (1.0 - t) * (1.0 - t);
        const auto h10 = t * (1.0 - t) * (1.0 - t);
        const auto h01 = t * t * (3.0 - 2.0 * t);
        const auto h11 = t * t * (t - 1.0);
        out[q] = h00 * y[i] + h10 * h[i] * m[i] + h01 * y[i + 1] + h11 * h[i] * m[i + 1];
    }
    return out;
}

double anchorOffsetDb (const std::vector<double>& freqsLog, const std::vector<double>& levelDb,
                       const TargetCurve& target, double lo, double hi)
{
    std::vector<double> f, level;
    for (std::size_t i = 0; i < freqsLog.size(); ++i)
        if (freqsLog[i] >= lo && freqsLog[i] <= hi && std::isfinite (levelDb[i]))
        {
            f.push_back (freqsLog[i]);
            level.push_back (levelDb[i]);
        }
    if (f.empty())
        return std::nan ("");
    const auto t = target.db (f);
    double sum = 0.0;
    for (std::size_t i = 0; i < f.size(); ++i)
        sum += level[i] - t[i];
    return sum / static_cast<double> (f.size());
}

std::vector<std::pair<double, double>> sanitizeTargetPoints (std::vector<std::pair<double, double>> points)
{
    for (auto& p : points)
    {
        p.first = std::clamp (std::isfinite (p.first) ? p.first : 1000.0, 20.0, 20000.0);
        p.second = std::clamp (std::isfinite (p.second) ? p.second : 0.0, -24.0, 24.0);
    }
    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> out;
    for (const auto& p : points)
        if (out.empty() || std::log2 (p.first / out.back().first) >= 1.0 / 48.0)
            out.push_back (p);
    if (out.empty())
        out.push_back ({ 1000.0, 0.0 });
    return out;
}
} // namespace roomeq
