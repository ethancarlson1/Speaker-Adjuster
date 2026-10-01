#include "roomeq/x32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace roomeq::x32
{
namespace
{
const double qRatio = std::log (minQ / maxQ);   // ln(0.03): from the top Q step to the bottom one

// printf with a '.' whatever the locale.
std::string format (const char* fmt, double v)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), fmt, v);
    std::string s (buf);
    std::replace (s.begin(), s.end(), ',', '.');
    return s;
}

double frequencyAt (int i) { return 20.0 * std::pow (10.0, 3.0 * i / (frequencySteps - 1)); }
int frequencyIndex (double hz)
{
    const auto i = std::lround ((frequencySteps - 1) * std::log10 (std::max (hz, 1.0) / 20.0) / 3.0);
    return static_cast<int> (std::clamp<long> (i, 0, frequencySteps - 1));
}

double qAt (int i) { return maxQ * std::exp (qRatio * i / (qSteps - 1)); }
double deskQ (double q)   // the step the desk snaps a Q it reads to
{
    const auto i = std::lround ((qSteps - 1) * std::log (std::max (q, 1e-3) / maxQ) / qRatio);
    return qAt (static_cast<int> (std::clamp<long> (i, 0, qSteps - 1)));
}

// The Qs a one-decimal value reaches, low to high.
const std::vector<double>& reachableQs()
{
    static const auto qs = []
    {
        std::vector<double> out;
        for (int i = qSteps - 1; i >= 0; --i)
        {
            const auto q = deskQ (std::strtod (qToken (qAt (i)).c_str(), nullptr));
            if (out.empty() || q > out.back() * (1.0 + 1e-12))
                out.push_back (q);
        }
        return out;
    }();
    return qs;
}

int qIndex (double q)
{
    const auto& qs = reachableQs();
    int best = 0;
    for (int i = 1; i < static_cast<int> (qs.size()); ++i)
        if (std::abs (std::log (qs[static_cast<std::size_t> (i)] / q)) < std::abs (std::log (qs[static_cast<std::size_t> (best)] / q)))
            best = i;
    return best;
}

constexpr int maxGainIndex = static_cast<int> (maxGainDb / gainStepDb);

// A band as indices into the desk's steps.
struct Steps
{
    int freq = 0, gain = 0, q = 0;
};

Band bandAt (const Steps& s)
{
    Band b;
    b.kind = BandKind::bell;
    b.freq = frequencyAt (s.freq);
    b.gainDb = s.gain * gainStepDb;
    b.q = reachableQs()[static_cast<std::size_t> (s.q)];
    return b;
}

std::vector<Band> bandsAt (const std::vector<Steps>& steps)
{
    std::vector<Band> out;
    for (const auto& s : steps)
        if (s.gain != 0)
            out.push_back (bandAt (s));
    std::stable_sort (out.begin(), out.end(), [] (const Band& a, const Band& b) { return a.freq < b.freq; });
    return out;
}

long long signed32 (long long v)
{
    return v >= (1LL << 31) ? v - (1LL << 32) : v;
}
} // namespace

std::string destinationName (Destination d)
{
    switch (d.strip)
    {
        case Strip::bus: return "Bus " + std::to_string (std::clamp (d.number, 1, 16));
        case Strip::matrix: return "Matrix " + std::to_string (std::clamp (d.number, 1, 6));
        case Strip::mainStereo: return "Main LR";
        case Strip::mainMono: return "Main M/C";
    }
    return {};
}

std::string stripPath (Destination d)
{
    const auto two = [] (int n)
    {
        char buf[8];
        std::snprintf (buf, sizeof (buf), "%02d", n);
        return std::string (buf);
    };
    switch (d.strip)
    {
        case Strip::bus: return "/bus/" + two (std::clamp (d.number, 1, 16));
        case Strip::matrix: return "/mtx/" + two (std::clamp (d.number, 1, 6));
        case Strip::mainStereo: return "/main/st";
        case Strip::mainMono: return "/main/m";
    }
    return {};
}

double snapFrequency (double hz) { return frequencyAt (frequencyIndex (hz)); }

double snapGain (double db)
{
    return std::clamp (std::round (db / gainStepDb), -static_cast<double> (maxGainIndex), static_cast<double> (maxGainIndex)) * gainStepDb;
}

double snapQ (double q) { return reachableQs()[static_cast<std::size_t> (qIndex (q))]; }

std::string frequencyToken (double hz)
{
    if (std::round (hz * 10.0) / 10.0 >= 1000.0)
    {
        auto s = format ("%.2f", hz / 1000.0);
        std::replace (s.begin(), s.end(), '.', 'k');
        return s;
    }
    return format ("%.1f", hz);
}

std::string gainToken (double db)
{
    if (std::abs (db) < 0.005)
        return "+0.00";
    return format (std::abs (db) < 10.0 ? "%+.2f" : "%+.1f", db);
}

std::string qToken (double q)
{
    return std::round (q * 10.0) / 10.0 >= 10.0 ? "10" : format ("%.1f", q);
}

Fit fitForDesk (const std::vector<Band>& correction, double fs)
{
    Fit fit;
    fit.originalBands = static_cast<int> (correction.size());
    if (correction.empty())
        return fit;
    const auto widest = bandwidthForQ (minQ);
    const auto refit = refitBands (correction, fs, numBands, false, widest);
    fit.refitted = refit.refitted;

    // On the desk's steps, then each value moved a step either way while that brings the curve closer.
    const auto prob = refitProblem (correction, fs, numBands, false, widest);
    std::vector<Steps> steps;
    for (const auto& b : refit.bands)
        steps.push_back ({ frequencyIndex (b.freq), static_cast<int> (std::lround (snapGain (b.gainDb) / gainStepDb)), qIndex (b.q) });
    const auto costOf = [&] (const std::vector<Steps>& s) { return fitCost (prob, responseDb (bandsAt (s), prob.freqs, fs)); };
    auto best = costOf (steps);
    const auto lastQ = static_cast<int> (reachableQs().size()) - 1;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool moved = false;
        for (std::size_t i = 0; i < steps.size(); ++i)
            for (int param = 0; param < 3; ++param)
                for (const auto delta : { -1, 1 })
                {
                    auto trial = steps;
                    auto& v = param == 0 ? trial[i].freq : param == 1 ? trial[i].gain : trial[i].q;
                    const auto [lo, hi] = param == 0 ? std::pair { 0, frequencySteps - 1 }
                                        : param == 1 ? std::pair { -maxGainIndex, maxGainIndex }
                                                     : std::pair { 0, lastQ };
                    v += delta;
                    if (v < lo || v > hi)
                        continue;
                    if (const auto c = costOf (trial); c < best - 1e-12)
                    {
                        best = c;
                        steps = std::move (trial);
                        moved = true;
                    }
                }
        if (! moved)
            break;
    }
    fit.bands = bandsAt (steps);
    std::tie (fit.maxErrorDb, fit.rmsErrorDb) = fitError (prob.desired, responseDb (fit.bands, prob.freqs, fs));
    return fit;
}

std::string snippet (const Fit& fit, Destination d, const std::string& name)
{
    std::string label;
    for (const auto c : name)
        if (c >= 0x20 && c < 0x7f && c != '"')
            label += c;
    label = label.substr (0, 16);
    while (! label.empty() && label.back() == ' ')
        label.pop_back();
    if (label.empty())
        label = "Room EQ";

    long long auxBuses = 0, mainGroups = 0;
    switch (d.strip)
    {
        case Strip::bus: auxBuses = 1LL << (15 + std::clamp (d.number, 1, 16)); break;
        case Strip::matrix: mainGroups = 1LL << (std::clamp (d.number, 1, 6) - 1); break;
        case Strip::mainStereo: mainGroups = 1LL << 6; break;
        case Strip::mainMono: mainGroups = 1LL << 7; break;
    }
    auto header = "#4.0# \"" + label + "\" 4 0 " + std::to_string (signed32 (auxBuses)) + " " + std::to_string (mainGroups) + " 1";
    if (header.size() < static_cast<std::size_t> (headerWidth))
        header.append (static_cast<std::size_t> (headerWidth) - header.size(), ' ');

    const auto path = stripPath (d);
    auto s = header + "\n" + path + "/eq ON\n";
    for (int k = 0; k < numBands; ++k)
    {
        Band b;   // past the fit's bands: flat
        b.freq = 1000.0;
        b.gainDb = 0.0;
        b.q = 2.0;
        if (k < static_cast<int> (fit.bands.size()))
            b = fit.bands[static_cast<std::size_t> (k)];
        s += path + "/eq/" + std::to_string (k + 1) + " PEQ " + frequencyToken (snapFrequency (b.freq)) + " "
             + gainToken (snapGain (b.gainDb)) + " " + qToken (snapQ (b.q)) + "\n";
    }
    return s;
}
} // namespace roomeq::x32
