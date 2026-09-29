#include "roomeq/correction.h"

#include "roomeq/capture.h"
#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace roomeq
{
namespace
{
const double nan = std::numeric_limits<double>::quiet_NaN();

double sign (double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }

// A band being fitted: log2(freq), gain dB and (bells only) log2(Q). The sign
// of the gain is fixed when the band is seeded.
struct Slot
{
    BandKind kind = BandKind::bell;
    double sgn = 1.0;
    std::vector<double> p;
};

Band fromParams (BandKind kind, const std::vector<double>& p)
{
    Band b;
    b.kind = kind;
    b.freq = std::exp2 (p[0]);
    b.gainDb = p[1];
    b.q = kind == BandKind::bell ? std::exp2 (p[2]) : shelfQ;
    return b;
}

std::vector<double> project (BandKind kind, std::vector<double> p, double sgn, const FitProblem& prob)
{
    p[0] = std::clamp (p[0], std::log2 (prob.fLo), std::log2 (prob.fHi));
    p[1] = sgn > 0.0 ? std::clamp (p[1], 0.0, prob.maxBoost) : std::clamp (p[1], -prob.maxCut, 0.0);
    if (kind == BandKind::bell)
    {
        const auto qLo = qForBandwidth (prob.cfg.maxOctaves);
        p[2] = std::clamp (p[2], std::log2 (qLo), std::log2 (maxQ (std::exp2 (p[0]), sgn, prob.cfg)));
    }
    return p;
}

std::vector<double> slotDb (const Slot& s, const FitProblem& prob)
{
    return bandDb (fromParams (s.kind, s.p), prob.freqs, prob.fs);
}

double cost (const FitProblem& prob, const std::vector<double>& c)
{
    double sum = 0.0;
    for (std::size_t n = 0; n < c.size(); ++n)
    {
        const auto e = c[n] - prob.desired[n];
        const auto over = std::max (0.0, c[n] - prob.upper[n]);
        const auto under = std::max (0.0, prob.lower[n] - c[n]);
        sum += prob.weight[n] * e * e + prob.cfg.penalty * (over * over + under * under);
    }
    return 0.5 * sum;
}

// Gaussian elimination with partial pivoting; a is n x n row-major.
std::vector<double> solve (std::vector<double> a, std::vector<double> b)
{
    const auto n = b.size();
    for (std::size_t col = 0; col < n; ++col)
    {
        auto piv = col;
        for (auto r = col + 1; r < n; ++r)
            if (std::abs (a[r * n + col]) > std::abs (a[piv * n + col]))
                piv = r;
        if (piv != col)
        {
            for (std::size_t k = 0; k < n; ++k)
                std::swap (a[col * n + k], a[piv * n + k]);
            std::swap (b[col], b[piv]);
        }
        const auto d = a[col * n + col];
        for (auto r = col + 1; r < n; ++r)
        {
            const auto f = a[r * n + col] / d;
            if (f == 0.0)
                continue;
            for (auto k = col; k < n; ++k)
                a[r * n + k] -= f * a[col * n + k];
            b[r] -= f * b[col];
        }
    }
    std::vector<double> x (n);
    for (auto r = n; r-- > 0;)
    {
        auto s = b[r];
        for (auto k = r + 1; k < n; ++k)
            s -= a[r * n + k] * x[k];
        x[r] = s / a[r * n + r];
    }
    return x;
}

// Refine slots[free] in place; the other bands contribute fixedDb. Bounded LM:
// steps are projected onto the bounds; lambda scales diag(J^T J). Returns the cost.
double levenbergMarquardt (std::vector<Slot>& slots, const std::vector<std::size_t>& free, const FitProblem& prob,
                           const std::vector<double>& fixedDb, int maxIter = 100)
{
    const auto nPts = prob.freqs.size();
    std::vector<std::vector<double>> responses;
    for (auto i : free)
        responses.push_back (slotDb (slots[i], prob));
    const auto sumCurve = [&] (const std::vector<std::vector<double>>& rs)
    {
        auto c = fixedDb;
        for (const auto& r : rs)
            for (std::size_t n = 0; n < nPts; ++n)
                c[n] += r[n];
        return c;
    };

    auto c = sumCurve (responses);
    auto cst = cost (prob, c);
    double lam = 1e-2;
    for (int iter = 0; iter < maxIter; ++iter)
    {
        // d(correction)/d(parameter), forward differences.
        std::vector<std::vector<double>> cols;
        for (std::size_t f = 0; f < free.size(); ++f)
        {
            const auto& s = slots[free[f]];
            for (std::size_t k = 0; k < s.p.size(); ++k)
            {
                constexpr double h = 1e-6;
                auto q = s.p;
                q[k] += h;
                auto d = bandDb (fromParams (s.kind, q), prob.freqs, prob.fs);
                for (std::size_t n = 0; n < nPts; ++n)
                    d[n] = (d[n] - responses[f][n]) / h;
                cols.push_back (std::move (d));
            }
        }
        const auto m = cols.size();
        // J^T J and J^T r without forming J (the penalty rows are active where c is past a limit).
        std::vector<double> rowWeight (nPts), rowResid (nPts);
        for (std::size_t n = 0; n < nPts; ++n)
        {
            const auto over = c[n] > prob.upper[n];
            const auto under = prob.lower[n] > c[n];
            rowWeight[n] = prob.weight[n] + (over ? prob.cfg.penalty : 0.0) + (under ? prob.cfg.penalty : 0.0);
            rowResid[n] = prob.weight[n] * (c[n] - prob.desired[n])
                          + prob.cfg.penalty * std::max (0.0, c[n] - prob.upper[n])
                          - prob.cfg.penalty * std::max (0.0, prob.lower[n] - c[n]);
        }
        std::vector<double> a (m * m, 0.0), g (m, 0.0);
        for (std::size_t j = 0; j < m; ++j)
        {
            for (std::size_t n = 0; n < nPts; ++n)
                g[j] += cols[j][n] * rowResid[n];
            for (std::size_t k = j; k < m; ++k)
            {
                double s = 0.0;
                for (std::size_t n = 0; n < nPts; ++n)
                    s += cols[j][n] * rowWeight[n] * cols[k][n];
                a[j * m + k] = a[k * m + j] = s;
            }
        }

        bool improved = false;
        double decrease = 0.0;
        while (lam < 1e10)
        {
            auto lhs = a;
            for (std::size_t j = 0; j < m; ++j)
                lhs[j * m + j] += lam * (a[j * m + j] + 1e-9);
            std::vector<double> rhs (m);
            for (std::size_t j = 0; j < m; ++j)
                rhs[j] = -g[j];
            const auto step = solve (std::move (lhs), std::move (rhs));

            std::vector<std::vector<double>> trial;
            std::vector<std::vector<double>> trialResp;
            std::size_t pos = 0;
            for (auto i : free)
            {
                const auto n = slots[i].p.size();
                auto p = slots[i].p;
                for (std::size_t k = 0; k < n; ++k)
                    p[k] += step[pos + k];
                pos += n;
                trial.push_back (project (slots[i].kind, std::move (p), slots[i].sgn, prob));
                trialResp.push_back (bandDb (fromParams (slots[i].kind, trial.back()), prob.freqs, prob.fs));
            }
            const auto cNew = sumCurve (trialResp);
            const auto costNew = cost (prob, cNew);
            if (costNew < cst)
            {
                for (std::size_t f = 0; f < free.size(); ++f)
                    slots[free[f]].p = trial[f];
                responses = std::move (trialResp);
                decrease = cst - costNew;
                c = cNew;
                cst = costNew;
                lam = std::max (lam / 3.0, 1e-9);
                improved = true;
                break;
            }
            lam *= 4.0;
        }
        if (! improved || decrease <= 1e-9 * std::max (cst, 1e-12))
            break;
    }
    return cst;
}

std::vector<bool> insideMask (const FitProblem& prob)
{
    std::vector<bool> inside (prob.freqs.size());
    for (std::size_t n = 0; n < inside.size(); ++n)
        inside[n] = prob.weight[n] > 0.0 && prob.freqs[n] >= prob.fLo && prob.freqs[n] <= prob.fHi;
    return inside;
}

std::optional<Slot> seedBell (const FitProblem& prob, const std::vector<double>& resid)
{
    const auto inside = insideMask (prob);
    const auto n = resid.size();
    struct Run { double area; std::size_t i, j; double sgn; };
    std::optional<Run> best;
    std::size_t i = 0;
    while (i < n)
    {
        if (! inside[i] || std::abs (resid[i]) < 0.1)
        {
            ++i;
            continue;
        }
        const auto sgn = sign (resid[i]);
        auto j = i;
        while (j + 1 < n && inside[j + 1] && sign (resid[j + 1]) == sgn && std::abs (resid[j + 1]) >= 0.1)
            ++j;
        double area = 0.0;
        for (auto k = i; k <= j; ++k)
            area += std::abs (resid[k]);
        area /= prob.cfg.pointsPerOctave;
        if (! best || area > best->area)
            best = Run { area, i, j, sgn };
        i = j + 1;
    }
    if (! best)
        return std::nullopt;

    auto k = best->i;
    for (auto t = best->i; t <= best->j; ++t)
        if (std::abs (resid[t]) > std::abs (resid[k]))
            k = t;
    const auto half = std::abs (resid[k]) / 2.0;
    auto a = k;
    while (a > best->i && std::abs (resid[a - 1]) >= half)
        --a;
    auto b = k;
    while (b < best->j && std::abs (resid[b + 1]) >= half)
        ++b;
    const auto octaves = std::max (std::log2 (prob.freqs[b]) - std::log2 (prob.freqs[a])
                                       + 1.0 / prob.cfg.pointsPerOctave, 0.1);
    Slot s { BandKind::bell, best->sgn, {} };
    s.p = project (BandKind::bell, { std::log2 (prob.freqs[k]), resid[k], std::log2 (qForBandwidth (octaves)) },
                   best->sgn, prob);
    return s;
}

// Candidate shelves at 1/3-octave steps, gain = mean residual on the shelved side.
std::vector<Slot> seedShelves (const FitProblem& prob, const std::vector<double>& resid, BandKind kind)
{
    const auto inside = insideMask (prob);
    std::vector<Slot> out;
    for (int step = 1; step < 30; ++step)
    {
        const auto f0 = prob.fLo * std::exp2 (step / 3.0);
        if (f0 >= prob.fHi)
            break;
        double sum = 0.0;
        int count = 0;
        for (std::size_t n = 0; n < resid.size(); ++n)
            if (inside[n] && (kind == BandKind::lowShelf ? prob.freqs[n] <= f0 : prob.freqs[n] >= f0))
            {
                sum += resid[n];
                ++count;
            }
        if (count < prob.cfg.pointsPerOctave / 2)
            continue;
        const auto g = sum / count;
        if (std::abs (g) < 0.3)
            continue;
        const auto sgn = sign (g);
        out.push_back ({ kind, sgn, project (kind, { std::log2 (f0), g }, sgn, prob) });
    }
    return out;
}

std::vector<double> curveOf (const std::vector<Slot>& slots, const FitProblem& prob)
{
    std::vector<double> c (prob.freqs.size(), 0.0);
    for (const auto& s : slots)
    {
        const auto r = slotDb (s, prob);
        for (std::size_t n = 0; n < c.size(); ++n)
            c[n] += r[n];
    }
    return c;
}

std::vector<std::size_t> allOf (std::size_t n)
{
    std::vector<std::size_t> idx (n);
    for (std::size_t i = 0; i < n; ++i)
        idx[i] = i;
    return idx;
}

// Included, non-redo captures, level-aligned and smoothed, redo bands masked.
std::vector<std::vector<double>> alignedPositions (const std::vector<std::shared_ptr<const Capture>>& captures,
                                                   const SessionSummary& summary, double fraction,
                                                   const std::vector<double>& grid)
{
    const auto& freqs = summary.freqs;
    double refFs = 0.0;
    for (const auto& c : captures)
        if (! c->excluded)
        {
            refFs = c->fs;
            break;
        }
    std::vector<std::vector<double>> out;
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        const auto& c = *captures[i];
        if (c.excluded || c.grade.overall == Grade::redo)
            continue;
        auto w = bandMaskWeights (c.freqs, c.grade);
        for (std::size_t k = 0; k < w.size(); ++k)
            w[k] *= c.weight[k];
        auto p = c.power;
        if (c.freqs.size() != freqs.size() || c.fs != refFs)
        {
            p = rebinPower (c.freqs, p, freqs);
            w = rebinPower (c.freqs, w, freqs);
        }
        const auto gain = std::pow (10.0, summary.offsetsDb[i] / 10.0);
        for (auto& v : p)
            v *= gain;
        out.push_back (toDb (smoothPower (freqs, p, fraction, grid, &w)));
    }
    return out;
}
} // namespace

double minCutOctaves (double freq, const CorrectionConfig& cfg)
{
    const auto a = std::log2 (cfg.cutSplitLoHz);
    const auto b = std::log2 (cfg.cutSplitHiHz);
    const auto t = std::clamp ((std::log2 (freq) - a) / (b - a), 0.0, 1.0);
    return cfg.cutMinOctavesLow + t * (cfg.cutMinOctavesHigh - cfg.cutMinOctavesLow);
}

double maxQ (double freq, double gainDb, const CorrectionConfig& cfg)
{
    return qForBandwidth (gainDb > 0.0 ? cfg.boostMinOctaves : minCutOctaves (freq, cfg));
}

std::vector<Band> fitBands (const FitProblem& prob)
{
    std::vector<Slot> slots;
    const std::vector<double> zero (prob.freqs.size(), 0.0);
    const auto inside = insideMask (prob);
    if (std::none_of (inside.begin(), inside.end(), [] (bool b) { return b; }))
        return {};

    auto cst = cost (prob, zero);
    for (int step = 0; step < prob.cfg.maxBands; ++step)
    {
        const auto c = curveOf (slots, prob);
        std::vector<double> resid (c.size());
        double sumSq = 0.0;
        int count = 0;
        for (std::size_t n = 0; n < c.size(); ++n)
        {
            resid[n] = prob.desired[n] - c[n];
            if (inside[n])
            {
                sumSq += resid[n] * resid[n];
                ++count;
            }
        }
        if (std::sqrt (sumSq / count) < prob.cfg.stopRmsDb)
            break;

        std::vector<Slot> candidates;
        if (auto bell = seedBell (prob, resid))
            candidates.push_back (*bell);
        for (auto kind : { BandKind::lowShelf, BandKind::highShelf })
            if (std::none_of (slots.begin(), slots.end(), [kind] (const Slot& s) { return s.kind == kind; }))
                for (auto& s : seedShelves (prob, resid, kind))
                    candidates.push_back (std::move (s));
        if (candidates.empty())
            break;

        // Refine each candidate on its own against the current bands; keep the best.
        std::optional<std::pair<double, Slot>> best;
        for (const auto& cand : candidates)
        {
            std::vector<Slot> trial { cand };
            const auto candCost = levenbergMarquardt (trial, { 0 }, prob, c, 30);
            if (! best || candCost < best->first)
                best = std::make_pair (candCost, trial.front());
        }
        if (! best || best->first > cst * (1.0 - prob.cfg.minImprovement))
            break;
        slots.push_back (best->second);
        cst = levenbergMarquardt (slots, allOf (slots.size()), prob, zero);
    }

    // Drop negligible bands, then refit what's left.
    std::vector<Slot> kept;
    for (const auto& s : slots)
        if (std::abs (s.p[1]) >= prob.cfg.minGainDb)
            kept.push_back (s);
    if (kept.size() != slots.size() && ! kept.empty())
        levenbergMarquardt (kept, allOf (kept.size()), prob, zero);

    std::vector<Band> bands;
    for (const auto& s : kept)
        bands.push_back (fromParams (s.kind, s.p));
    std::stable_sort (bands.begin(), bands.end(), [] (const Band& a, const Band& b) { return a.freq < b.freq; });
    return bands;
}

std::vector<bool> detectNulls (const std::vector<double>& grid, const std::vector<double>& fineDb,
                               const std::vector<double>& trendDb, const std::vector<std::vector<double>>& positions,
                               const CorrectionConfig& cfg)
{
    const auto n = grid.size();
    std::vector<bool> null (n, false);
    for (std::size_t i = 0; i < n; ++i)
        null[i] = fineDb[i] - trendDb[i] < -cfg.nullDipDb;
    if (positions.size() >= 2)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            int valid = 0;
            auto hi = -std::numeric_limits<double>::infinity();
            auto lo = std::numeric_limits<double>::infinity();
            for (const auto& p : positions)
                if (std::isfinite (p[i]))
                {
                    ++valid;
                    hi = std::max (hi, p[i]);
                    lo = std::min (lo, p[i]);
                }
            if (valid >= 2 && hi - lo > cfg.nullSpreadDb)
                null[i] = true;
        }
    }
    const auto margin = static_cast<std::ptrdiff_t> (std::lround (cfg.nullMarginOctaves * cfg.pointsPerOctave));
    if (margin > 0)
    {
        std::vector<bool> wide (n, false);
        for (std::size_t i = 0; i < n; ++i)
            if (null[i])
                for (auto k = std::max<std::ptrdiff_t> (0, static_cast<std::ptrdiff_t> (i) - margin);
                     k <= static_cast<std::ptrdiff_t> (i) + margin && k < static_cast<std::ptrdiff_t> (n); ++k)
                    wide[static_cast<std::size_t> (k)] = true;
        null = wide;
    }
    return null;
}

CorrectionResult designCorrection (const std::vector<std::shared_ptr<const Capture>>& captures,
                                   const SessionSummary& summary, const TargetCurve& target, double fs,
                                   const CorrectionConfig& cfg)
{
    CorrectionResult r;
    r.grid = logFreqGrid (20.0, 20000.0, cfg.pointsPerOctave);
    const auto& g = r.grid;
    const auto n = g.size();
    const auto& policy = summary.policy;
    r.averageDb = toDb (smoothPower (summary.freqs, summary.power, policy.smoothingFraction, g, &summary.weight));
    const auto fine = toDb (smoothPower (summary.freqs, summary.power, 6.0, g, &summary.weight));
    r.trendDb = toDb (smoothPower (summary.freqs, summary.power, 1.0, g, &summary.weight));

    auto [lo, hi] = usableRange (g, r.trendDb, cfg.refBandLoHz, cfg.refBandHiHz, cfg.rolloffDb);
    lo = std::max (lo, cfg.rangeLoHz);
    hi = std::min (hi, cfg.rangeHiHz);
    r.fitRange = { lo, hi };

    const auto offset = anchorOffsetDb (g, r.averageDb, target, cfg.refBandLoHz, cfg.refBandHiHz);
    r.targetDb = target.db (g);
    for (auto& v : r.targetDb)
        v += offset;

    r.strength = policy.strength;
    auto maxCut = cfg.maxCutDb;
    auto maxBoost = cfg.maxBoostDb;
    if (policy.maxCorrectionDb)
    {
        maxCut = std::min (maxCut, *policy.maxCorrectionDb / r.strength);
        maxBoost = std::min (maxBoost, *policy.maxCorrectionDb / r.strength);
    }

    r.nullMask = detectNulls (g, fine, r.trendDb, alignedPositions (captures, summary, 3.0, g), cfg);

    FitProblem prob;
    prob.freqs = g;
    prob.fs = fs;
    prob.fLo = lo;
    prob.fHi = hi;
    prob.maxCut = maxCut;
    prob.maxBoost = maxBoost;
    prob.cfg = cfg;
    prob.desired.resize (n);
    prob.weight.resize (n);
    prob.upper.resize (n);
    prob.lower.assign (n, -maxCut);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto inside = g[i] >= lo && g[i] <= hi;
        const auto finite = std::isfinite (r.averageDb[i]);
        prob.upper[i] = inside && ! r.nullMask[i] ? maxBoost : 0.0;
        auto want = r.targetDb[i] - r.averageDb[i];
        if (std::isnan (want))
            want = 0.0;
        prob.desired[i] = inside && finite ? std::clamp (want, prob.lower[i], prob.upper[i]) : 0.0;
        prob.weight[i] = inside ? (finite ? 1.0 : 0.0) : cfg.outsideWeight;
    }
    r.desiredDb = prob.desired;
    r.upperDb = prob.upper;
    r.lowerDb = prob.lower;

    r.fitted = fitBands (prob);
    for (const auto& b : r.fitted)
        r.bands.push_back (b.scaled (r.strength));
    r.correctionDb = responseDb (r.bands, g, fs);
    r.predictedDb.resize (n);
    double sumSq = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        r.predictedDb[i] = r.averageDb[i] + r.correctionDb[i];
        if (g[i] >= lo && g[i] <= hi && ! r.nullMask[i] && std::isfinite (r.averageDb[i]))
        {
            const auto e = r.predictedDb[i] - r.targetDb[i];
            sumSq += e * e;
            ++count;
        }
    }
    r.rmsErrorDb = count > 0 ? std::sqrt (sumSq / count) : nan;
    return r;
}
} // namespace roomeq
