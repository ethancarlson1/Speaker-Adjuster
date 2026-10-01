#pragma once

// Refit an EQ curve with fewer bands, for an output that has only N.
// Port of prototype/roomeq/refit.py.
//
// A console's output EQ often has 4-8 parametric bands; the correction can use
// up to 10. Rather than drop the smallest, the curve the correction plays is
// fitted again with at most N bands, by the correction's own band fit with
// that curve as the wanted correction:
//
// - the grid is 20 Hz-20 kHz at 24 points per octave, every point weighted the
//   same (the curve is known everywhere, and flat outside the corrected range);
// - never boost more than the curve does: the refit may exceed the curve's
//   boost (or 0 dB where it cuts) by at most 0.5 dB, and cut at most 3 dB more;
// - each band's gain stays within the curve's largest boost + 1 dB and its
//   largest cut + 3 dB (both at most 15 dB); widths are the correction's;
// - shelves are optional (consoles define a shelf's Q differently), and the
//   widest bell is a setting (consoles go to Q 0.3, about 3.7 octaves).
//
// Two fits, and the closer one is kept (by the fit's own cost): the greedy
// forward fit (fitBands) and the backward one (pruneBands), which keeps a
// narrow, deep cut the forward fit would merge into a broad one.
//
// Bands that already fit (no more than N, and no shelf where none is allowed)
// are kept as they are. The fit error compares the refit with the curve: the
// largest difference on the grid, and the RMS over the points where either is
// beyond 0.1 dB.

#include "roomeq/filters.h"

#include <utility>
#include <vector>

namespace roomeq
{
inline constexpr int refitPointsPerOctave = 24;
inline constexpr double refitBoostSlackDb = 0.5;      // above the curve's boost (or 0 dB)
inline constexpr double refitCutSlackDb = 3.0;        // below the curve's cut (or 0 dB)
inline constexpr double refitBoostHeadroomDb = 1.0;   // a band's boost beyond the curve's largest
inline constexpr double refitGainLimitDb = 15.0;
inline constexpr double refitActiveDb = 0.1;          // RMS error over the points where either curve is beyond this

struct Refit
{
    std::vector<Band> bands;
    bool refitted = false;       // false: the bands already fitted and are unchanged
    int originalBands = 0;
    double maxErrorDb = 0.0;     // refit vs curve, anywhere on the grid
    double rmsErrorDb = 0.0;     // over the points where either is beyond refitActiveDb
};

std::vector<double> refitGrid();

// (largest difference, RMS over the points where either curve is beyond refitActiveDb).
std::pair<double, double> fitError (const std::vector<double>& curveDb, const std::vector<double>& fittedDb);

// At most maxBands bands that play `bands`' curve as closely as they can.
// maxOctaves: the widest bell (the correction's 3 octaves by default).
Refit refitBands (const std::vector<Band>& bands, double fs, int maxBands, bool shelves = true, double maxOctaves = 3.0);
} // namespace roomeq
