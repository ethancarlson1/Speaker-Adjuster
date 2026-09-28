#include "roomeq/loudness.h"

#include "roomeq/dualfft.h"
#include "roomeq/iso226.h"
#include "roomeq/spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace roomeq
{
namespace
{
constexpr double hpC = 20.598997, lpC = 12194.217;
constexpr double butterQ1 = 0.5411961, butterQ2 = 1.3065630;

std::vector<double> geomspace (double a, double b, int n)
{
    std::vector<double> out (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t> (i)] = a * std::pow (b / a, static_cast<double> (i) / (n - 1));
    out.back() = b;
    return out;
}

double sumSq (const std::vector<double>& a, const std::vector<double>& b)
{
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        s += (a[i] - b[i]) * (a[i] - b[i]);
    return s;
}

// Least-squares gain for a shelf of fixed frequency/Q (its dB response scales
// almost exactly with gain: one linear solve plus one refinement).
double fitGain (BandKind kind, double freq, double q, const std::vector<double>& target, const std::vector<double>& grid,
                double fs)
{
    double g = 1.0;
    for (int it = 0; it < 2; ++it)
    {
        const auto r = bandDb ({ kind, freq, g, q }, grid, fs);
        double num = 0.0, den = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i)
        {
            const auto shape = r[i] / g;
            num += shape * target[i];
            den += shape * shape;
        }
        const auto gNew = num / den;
        if (std::abs (gNew) < 1e-6)
            return 0.0;
        g = gNew;
    }
    return std::max (g, 0.0);
}

double interpGain (const std::array<double, ShelfPlan::numDeltas>& table, double delta)
{
    const auto d = std::clamp (delta, 0.0, static_cast<double> (ShelfPlan::numDeltas - 1));
    const auto i = std::min (static_cast<int> (d), ShelfPlan::numDeltas - 2);
    const auto t = d - i;
    return table[static_cast<std::size_t> (i)] * (1.0 - t) + table[static_cast<std::size_t> (i + 1)] * t;
}
} // namespace

std::array<Band, 2> cWeightingBands()
{
    return { Band { BandKind::highPass, hpC, 0.0, 0.5 }, Band { BandKind::lowPass, lpC, 0.0, 0.5 } };
}

double cWeightingGain (double fs)
{
    const auto b = cWeightingBands();
    return std::pow (10.0, -(bandDb (b[0], { 1000.0 }, fs)[0] + bandDb (b[1], { 1000.0 }, fs)[0]) / 20.0);
}

double cWeightedLevelDbfs (const std::vector<double>& x, double fs)
{
    const auto b = cWeightingBands();
    const auto c1 = designBiquad (b[0], fs);
    const auto c2 = designBiquad (b[1], fs);
    const auto g = cWeightingGain (fs);
    double s11 = 0, s12 = 0, s21 = 0, s22 = 0, acc = 0;
    for (auto v : x)
    {
        const auto in = g * v;
        const auto y1 = c1.b0 * in + s11;
        s11 = c1.b1 * in - c1.a1 * y1 + s12;
        s12 = c1.b2 * in - c1.a2 * y1;
        const auto y2 = c2.b0 * y1 + s21;
        s21 = c2.b1 * y1 - c2.a1 * y2 + s22;
        s22 = c2.b2 * y1 - c2.a2 * y2;
        acc += y2 * y2;
    }
    return 10.0 * std::log10 (acc / static_cast<double> (std::max<std::size_t> (x.size(), 1)) + 1e-30);
}

std::vector<double> compensationTarget (const std::vector<double>& freqs, double currentSpl, double referenceSpl)
{
    if (currentSpl >= referenceSpl)
        return std::vector<double> (freqs.size(), 0.0);
    const auto now = std::max (currentSpl, 20.0);   // the contours aren't defined below 20 phon
    auto out = iso226RelativeContour (freqs, now);
    const auto ref = iso226RelativeContour (freqs, referenceSpl);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] -= ref[i];
    return out;
}

ShelfPlan planShelves (double referenceSpl, double fs)
{
    constexpr double fitDelta = 15.0;
    const auto lowGrid = geomspace (20.0, 1000.0, 60);
    const auto highGrid = geomspace (2000.0, 16000.0, 30);
    const auto tLow = compensationTarget (lowGrid, referenceSpl - fitDelta, referenceSpl);
    const auto tHigh = compensationTarget (highGrid, referenceSpl - fitDelta, referenceSpl);

    ShelfPlan plan;
    plan.referenceSpl = referenceSpl;
    auto best = std::numeric_limits<double>::infinity();
    for (auto f0 : geomspace (60.0, 600.0, 41))
        for (double q : { 0.4, 0.5, 0.6, 0.7071 })
        {
            const auto g = fitGain (BandKind::lowShelf, f0, q, tLow, lowGrid, fs);
            const auto err = sumSq (bandDb ({ BandKind::lowShelf, f0, g, q }, lowGrid, fs), tLow);
            if (err < best)
            {
                best = err;
                plan.lowFreq = f0;
                plan.lowQ = q;
            }
        }
    best = std::numeric_limits<double>::infinity();
    for (auto f0 : geomspace (4000.0, 16000.0, 25))
    {
        const auto g = fitGain (BandKind::highShelf, f0, shelfQ, tHigh, highGrid, fs);
        const auto err = sumSq (bandDb ({ BandKind::highShelf, f0, g, shelfQ }, highGrid, fs), tHigh);
        if (err < best)
        {
            best = err;
            plan.highFreq = f0;
        }
    }
    plan.highQ = shelfQ;
    for (int d = 0; d < ShelfPlan::numDeltas; ++d)
    {
        const auto i = static_cast<std::size_t> (d);
        if (d == 0)
            continue;
        plan.lowGain[i] = fitGain (BandKind::lowShelf, plan.lowFreq, plan.lowQ,
                                   compensationTarget (lowGrid, referenceSpl - d, referenceSpl), lowGrid, fs);
        plan.highGain[i] = fitGain (BandKind::highShelf, plan.highFreq, plan.highQ,
                                    compensationTarget (highGrid, referenceSpl - d, referenceSpl), highGrid, fs);
    }
    return plan;
}

std::pair<double, double> shelfGains (const ShelfPlan& plan, double currentSpl, const LoudnessConfig& cfg)
{
    const auto delta = plan.referenceSpl - currentSpl;
    if (! (delta > 0.0))
        return { 0.0, 0.0 };
    const auto low = interpGain (plan.lowGain, delta) * cfg.amount;
    const auto high = interpGain (plan.highGain, delta) * cfg.amount;
    return { std::min ({ low, cfg.maxLowDb, cfg.lowCeilingDb }), std::min (high, cfg.maxHighDb) };
}

std::array<Band, 2> trackingHighpass (double baseHz, double lowGainDb, const LoudnessConfig& cfg)
{
    const auto top = std::max (std::min (cfg.maxLowDb, cfg.lowCeilingDb), 1e-6);
    const auto fc = baseHz * std::exp2 (cfg.hpRiseOctaves * std::clamp (lowGainDb / top, 0.0, 1.0));
    return { Band { BandKind::highPass, fc, 0.0, butterQ1 }, Band { BandKind::highPass, fc, 0.0, butterQ2 } };
}

// ---------------------------------------------------------------------------

void LevelTracker::prepare (double fs, const LoudnessConfig& cfg)
{
    config = cfg;
    const auto b = cWeightingBands();
    hp = designBiquad (b[0], fs);
    lp = designBiquad (b[1], fs);
    gain = cWeightingGain (fs);
    window = std::max (1L, std::lround (cfg.windowS * fs));
    reset();
}

void LevelTracker::reset() noexcept
{
    hp1 = hp2 = lp1 = lp2 = 0.0;
    count = 0;
    acc = 0.0;
    has = active = false;
    value = 0.0;
    gatedFor = 0.0;
}

int LevelTracker::process (const float* x, int numSamples, const LevelTracker* follow) noexcept
{
    int windows = 0;
    for (int n = 0; n < numSamples; ++n)
    {
        const auto in = gain * static_cast<double> (x[n]);
        const auto y1 = hp.b0 * in + hp1;
        hp1 = hp.b1 * in - hp.a1 * y1 + hp2;
        hp2 = hp.b2 * in - hp.a2 * y1;
        const auto y2 = lp.b0 * y1 + lp1;
        lp1 = lp.b1 * y1 - lp.a1 * y2 + lp2;
        lp2 = lp.b2 * y1 - lp.a2 * y2;
        acc += y2 * y2;
        if (++count == window)
        {
            const auto momentary = 10.0 * std::log10 (acc / static_cast<double> (window) + 1e-30);
            if (follow == nullptr)
                active = update (momentary, true);
            else if (follow->lastActive())
                update (momentary, false);
            acc = 0.0;
            count = 0;
            ++windows;
        }
    }
    return windows;
}

bool LevelTracker::update (double momentary, bool gates) noexcept
{
    const auto dt = config.windowS;
    if (gates && momentary < config.absGateDbfs)
        return false;                                            // silence: hold
    if (! has)
    {
        value = momentary;
        has = true;
        return true;
    }
    if (gates && momentary < value - config.relGateDb)
    {
        gatedFor += dt;
        if (gatedFor < config.pauseHoldS)
            return false;                                        // a pause between songs: hold
    }
    else
    {
        gatedFor = 0.0;
    }
    if (momentary > value)
    {
        const auto a = 1.0 - std::exp (-dt / config.attackS);
        value = 10.0 * std::log10 ((1.0 - a) * std::pow (10.0, value / 10.0) + a * std::pow (10.0, momentary / 10.0));
    }
    else
    {
        const auto a = 1.0 - std::exp (-dt / config.speedS);
        value += a * (momentary - value);
    }
    return true;
}

double Deadband::update (double estimate, const LoudnessConfig& cfg) noexcept
{
    if (! has)
    {
        held = estimate;
        has = true;
    }
    else if (estimate > held + cfg.deadbandDb)
        held = estimate - cfg.deadbandDb;
    else if (estimate < held - cfg.deadbandDb)
        held = estimate + cfg.deadbandDb;
    else
        held += (1.0 - std::exp (-cfg.windowS / cfg.driftS)) * (estimate - held);
    return held;
}

// ---------------------------------------------------------------------------

const std::vector<double>& recheckBands()
{
    static const std::vector<double> bands = []
    {
        std::vector<double> b;
        for (int k = 0; k < 22; ++k)
            b.push_back (63.0 * std::exp2 (k / 3.0));
        return b;
    }();
    return bands;
}

std::vector<double> transferBandsDb (const std::vector<double>& output, const std::vector<double>& mic, double fs,
                                     double minCoherence)
{
    const auto est = transferFunction (output, mic, fs);
    const auto gate = excitationGate (est);
    const auto& bands = recheckBands();
    std::vector<double> out (bands.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t b = 0; b < bands.size(); ++b)
    {
        const auto lo = bands[b] * std::exp2 (-1.0 / 6.0);
        const auto hi = bands[b] * std::exp2 (1.0 / 6.0);
        int n = 0;
        double cohSum = 0.0, h2Sum = 0.0;
        for (std::size_t k = 0; k < est.freqs.size(); ++k)
        {
            if (est.freqs[k] < lo || est.freqs[k] >= hi || gate[k] <= 0.0)
                continue;
            ++n;
            cohSum += est.coherence[k];
            // |H1|^2 averaged over a band reads high by incoherent power / K: remove it.
            const auto coherent = std::norm (est.H[k]) * est.gxx[k];
            const auto incoherent = std::max (est.gyy[k] - coherent, 0.0);
            h2Sum += std::max (coherent - incoherent / est.segments, 0.0) / est.gxx[k];
        }
        if (n < 3 || cohSum / n < minCoherence || h2Sum <= 0.0)
            continue;
        out[b] = 10.0 * std::log10 (h2Sum / n);
    }
    return out;
}

std::optional<double> recheckGainChange (const std::vector<double>& calibrationBands, const std::vector<double>& nowBands,
                                         int minBands)
{
    std::vector<double> diffs;
    for (std::size_t i = 0; i < calibrationBands.size() && i < nowBands.size(); ++i)
        if (std::isfinite (calibrationBands[i]) && std::isfinite (nowBands[i]))
            diffs.push_back (nowBands[i] - calibrationBands[i]);
    if (static_cast<int> (diffs.size()) < minBands)
        return std::nullopt;
    std::sort (diffs.begin(), diffs.end());
    const auto m = diffs.size() / 2;
    return diffs.size() % 2 == 1 ? diffs[m] : 0.5 * (diffs[m - 1] + diffs[m]);
}

Calibration recalibrated (const Calibration& cal, double gainChangeDb)
{
    auto c = cal;
    c.outputDbfs -= gainChangeDb;
    return c;
}
} // namespace roomeq
