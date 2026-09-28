#include "roomeq/averaging.h"

#include "roomeq/capture.h"
#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roomeq
{
namespace
{
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
}

std::vector<double> bandMaskWeights (const std::vector<double>& freqs, const CaptureGrade& grade)
{
    std::vector<double> logCenters, bandWeight;
    for (std::size_t j = 0; j < grade.bands.size(); ++j)
    {
        logCenters.push_back (std::log2 (octaveCenters[j]));
        bandWeight.push_back (grade.bands[j].grade() == Grade::redo ? 0.0 : 1.0);
    }
    std::vector<double> w (freqs.size());
    for (std::size_t k = 0; k < freqs.size(); ++k)
        w[k] = interp (std::log2 (std::max (freqs[k], 1e-3)), logCenters, bandWeight);
    return w;
}

double levelOffsetDb (const std::vector<double>& freqs, const std::vector<double>& power,
                      double lo, double hi, const std::vector<double>* weights)
{
    return nanMean (toDb (smoothPower (freqs, power, 3.0, logFreqGrid (lo, hi, 24), weights)));
}

AverageResult powerAverage (const std::vector<double>& freqs, const std::vector<std::vector<double>>& powers,
                            const std::vector<std::vector<double>>& weights, bool alignLevels, double lo, double hi)
{
    AverageResult r;
    r.freqs = freqs;
    r.offsetsDb.assign (powers.size(), 0.0);
    if (alignLevels && ! powers.empty())
    {
        std::vector<double> levels;
        for (std::size_t i = 0; i < powers.size(); ++i)
            levels.push_back (levelOffsetDb (freqs, powers[i], lo, hi, &weights[i]));
        const auto target = nanMean (levels);
        for (std::size_t i = 0; i < powers.size(); ++i)
            r.offsetsDb[i] = std::isnan (levels[i]) ? 0.0 : target - levels[i];
    }

    std::vector<double> num (freqs.size(), 0.0);
    r.weight.assign (freqs.size(), 0.0);
    for (std::size_t i = 0; i < powers.size(); ++i)
    {
        const auto gain = std::pow (10.0, r.offsetsDb[i] / 10.0);
        for (std::size_t k = 0; k < freqs.size(); ++k)
        {
            num[k] += weights[i][k] * powers[i][k] * gain;
            r.weight[k] += weights[i][k];
        }
    }
    r.power.resize (freqs.size());
    for (std::size_t k = 0; k < freqs.size(); ++k)
        r.power[k] = r.weight[k] > 0.0 ? num[k] / std::max (r.weight[k], 1e-30) : nan;
    return r;
}

std::pair<double, double> usableRange (const std::vector<double>& freqsLog, const std::vector<double>& levelDb,
                                       double refLo, double refHi, double dropDb, double startHz)
{
    std::vector<double> ref;
    for (std::size_t i = 0; i < freqsLog.size(); ++i)
        if (freqsLog[i] >= refLo && freqsLog[i] <= refHi)
            ref.push_back (levelDb[i]);
    const auto threshold = nanMean (ref) - dropDb;

    std::size_t i0 = 0;
    for (std::size_t i = 1; i < freqsLog.size(); ++i)
        if (std::abs (freqsLog[i] - startHz) < std::abs (freqsLog[i0] - startHz))
            i0 = i;

    auto lo = freqsLog.front();
    auto hi = freqsLog.back();
    for (std::size_t i = i0 + 1; i-- > 0;)
    {
        if (! (levelDb[i] >= threshold))   // NaN counts as below
        {
            lo = freqsLog[std::min (i + 1, freqsLog.size() - 1)];
            break;
        }
    }
    for (std::size_t i = i0; i < freqsLog.size(); ++i)
    {
        if (! (levelDb[i] >= threshold))
        {
            hi = freqsLog[i > 0 ? i - 1 : 0];
            break;
        }
    }
    return { lo, hi };
}

double targetLevelDb (const std::vector<double>& freqsLog, const std::vector<double>& levelDb, double lo, double hi)
{
    std::vector<double> sel;
    for (std::size_t i = 0; i < freqsLog.size(); ++i)
        if (freqsLog[i] >= lo && freqsLog[i] <= hi)
            sel.push_back (levelDb[i]);
    return nanMean (sel);
}

QuickModePolicy quickModePolicy (int nGood, int userFraction)
{
    if (nGood <= 1)
        return { std::min (userFraction, 2), 0.5, 3.0 };
    if (nGood == 2)
        return { std::min (userFraction, 3), 0.75, 6.0 };
    return { userFraction, 1.0, std::nullopt };
}

std::optional<SessionSummary> summarizeSession (const std::vector<std::shared_ptr<const Capture>>& captures,
                                                int userFraction, const std::vector<double>& grid)
{
    std::vector<std::size_t> included;
    for (std::size_t i = 0; i < captures.size(); ++i)
        if (! captures[i]->excluded)
            included.push_back (i);
    if (included.empty())
        return std::nullopt;

    const auto& ref = *captures[included.front()];
    const auto& freqs = ref.freqs;
    const auto onGrid = [&] (const Capture& c, const std::vector<double>& values)
    {
        if (c.freqs.size() == freqs.size() && c.fs == ref.fs)
            return values;
        return rebinPower (c.freqs, values, freqs);
    };

    std::vector<std::vector<double>> powers, maskWeights, displayWeights;
    for (const auto& c : captures)
    {
        auto w = bandMaskWeights (c->freqs, c->grade);
        for (std::size_t k = 0; k < w.size(); ++k)
            w[k] *= c->weight[k];
        powers.push_back (onGrid (*c, c->power));
        maskWeights.push_back (onGrid (*c, w));
        displayWeights.push_back (onGrid (*c, c->weight));
    }

    // Align every capture (excluded ones too, for display) to the mean level of the included ones.
    std::vector<double> levels, includedLevels;
    for (std::size_t i = 0; i < captures.size(); ++i)
        levels.push_back (levelOffsetDb (freqs, powers[i], 250.0, 4000.0, &maskWeights[i]));
    for (auto i : included)
        includedLevels.push_back (levels[i]);
    const auto target = nanMean (includedLevels);

    SessionSummary s;
    s.freqs = freqs;
    for (auto level : levels)
        s.offsetsDb.push_back (std::isnan (level) || std::isnan (target) ? 0.0 : target - level);

    std::vector<double> num (freqs.size(), 0.0);
    s.weight.assign (freqs.size(), 0.0);
    for (auto i : included)
    {
        const auto gain = std::pow (10.0, s.offsetsDb[i] / 10.0);
        for (std::size_t k = 0; k < freqs.size(); ++k)
        {
            num[k] += maskWeights[i][k] * powers[i][k] * gain;
            s.weight[k] += maskWeights[i][k];
        }
    }
    s.power.resize (freqs.size());
    for (std::size_t k = 0; k < freqs.size(); ++k)
        s.power[k] = s.weight[k] > 0.0 ? num[k] / std::max (s.weight[k], 1e-30) : nan;

    for (auto i : included)
        if (captures[i]->grade.overall != Grade::redo)
            ++s.nGood;
    s.policy = quickModePolicy (s.nGood, userFraction);

    s.grid = grid;
    const auto fraction = static_cast<double> (s.policy.smoothingFraction);
    s.averageDb = toDb (smoothPower (freqs, s.power, fraction, grid, &s.weight));
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        auto p = powers[i];
        const auto gain = std::pow (10.0, s.offsetsDb[i] / 10.0);
        for (auto& v : p)
            v *= gain;
        s.positionDb.push_back (toDb (smoothPower (freqs, p, fraction, grid, &displayWeights[i])));
    }
    s.usable = usableRange (grid, toDb (smoothPower (freqs, s.power, 1.0, grid, &s.weight)));
    s.targetDb = targetLevelDb (grid, s.averageDb);
    return s;
}
} // namespace roomeq
