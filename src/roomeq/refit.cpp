#include "roomeq/refit.h"

#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace roomeq
{
std::vector<double> refitGrid()
{
    return logFreqGrid (20.0, 20000.0, refitPointsPerOctave);
}

std::pair<double, double> fitError (const std::vector<double>& curveDb, const std::vector<double>& fittedDb)
{
    double largest = 0.0, sumSq = 0.0;
    int active = 0;
    for (std::size_t n = 0; n < curveDb.size(); ++n)
    {
        const auto d = fittedDb[n] - curveDb[n];
        largest = std::max (largest, std::abs (d));
        if (std::abs (curveDb[n]) > refitActiveDb || std::abs (fittedDb[n]) > refitActiveDb)
        {
            sumSq += d * d;
            ++active;
        }
    }
    return { largest, active > 0 ? std::sqrt (sumSq / active) : 0.0 };
}

FitProblem refitProblem (const std::vector<Band>& bands, double fs, int maxBands, bool shelves, double maxOctaves,
                         double narrowestOctaves)
{
    const auto grid = refitGrid();
    const auto curve = responseDb (bands, grid, fs);
    const auto [lowest, highest] = std::minmax_element (curve.begin(), curve.end());
    const auto maxBoost = std::min (refitGainLimitDb, std::max (0.0, *highest) + refitBoostHeadroomDb);
    const auto maxCut = std::min (refitGainLimitDb, std::max (0.0, -*lowest) + refitCutSlackDb);

    FitProblem prob;
    prob.freqs = grid;
    prob.fs = fs;
    prob.desired = curve;
    prob.weight.assign (grid.size(), 1.0);
    prob.upper.resize (grid.size());
    prob.lower.resize (grid.size());
    for (std::size_t n = 0; n < grid.size(); ++n)
    {
        prob.upper[n] = std::max (curve[n], 0.0) + refitBoostSlackDb;
        prob.lower[n] = std::min (curve[n], 0.0) - refitCutSlackDb;
    }
    prob.fLo = 20.0;
    prob.fHi = 20000.0;
    prob.maxCut = maxCut;
    prob.maxBoost = maxBoost;
    prob.cfg.maxBands = maxBands;
    prob.cfg.maxCutDb = maxCut;
    prob.cfg.maxBoostDb = maxBoost;
    prob.cfg.maxOctaves = maxOctaves;
    prob.cfg.shelves = shelves;
    prob.cfg.outsideWeight = 1.0;
    prob.cfg.minGainDb = 0.1;
    prob.cfg.minImprovement = 0.005;
    prob.cfg.stopRmsDb = 0.02;
    if (narrowestOctaves > 0.0)
        prob.cfg.boostMinOctaves = prob.cfg.cutMinOctavesLow = prob.cfg.cutMinOctavesHigh = narrowestOctaves;
    return prob;
}

Refit refitBands (const std::vector<Band>& bands, double fs, int maxBands, bool shelves, double maxOctaves,
                  double narrowestOctaves)
{
    const auto hasShelf = std::any_of (bands.begin(), bands.end(), [] (const Band& b) { return b.kind != BandKind::bell; });
    if (static_cast<int> (bands.size()) <= maxBands && (shelves || ! hasShelf))
        return { bands, false, static_cast<int> (bands.size()), 0.0, 0.0 };

    const auto prob = refitProblem (bands, fs, maxBands, shelves, maxOctaves, narrowestOctaves);
    const auto forward = maxBands > 0 ? fitBands (prob) : std::vector<Band> {};
    const auto backward = pruneBands (bands, prob, maxBands);
    const auto costForward = fitCost (prob, responseDb (forward, prob.freqs, fs));
    const auto costBackward = fitCost (prob, responseDb (backward, prob.freqs, fs));
    Refit r;
    r.bands = costBackward < costForward ? backward : forward;
    r.refitted = true;
    r.originalBands = static_cast<int> (bands.size());
    std::tie (r.maxErrorDb, r.rmsErrorDb) = fitError (prob.desired, responseDb (r.bands, prob.freqs, fs));
    return r;
}
} // namespace roomeq
