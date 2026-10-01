#pragma once

// The correction as a Behringer X32 / Midas M32 snippet (.snp): a file the
// desk (or X32-Edit / M32-Edit) loads to set one output strip's EQ, leaving
// the rest of the desk as it is.
//
// Mix buses, matrices and the main LR and M/C strips have six EQ bands. The
// correction, as it plays, is refitted to six bells (refit.h: no shelves, as
// a console's shelf Q is its own; the widest bell is the desk's Q 0.3), then
// every value is put on the desk's own steps and the steps nearest each value
// searched for the closest curve:
//
// - frequency: 201 steps, log-spaced 20 Hz-20 kHz, written "85.3" below
//   1 kHz and "4k53" above (k for the decimal point);
// - gain: -15 to +15 dB in 0.25 dB steps, written "+4.75" ("+10.2" from 10 dB);
// - Q: 72 steps, log-spaced 10 to 0.3, written with one decimal ("2.0", and
//   "10" at the top), as the desk writes them; so below Q 1 only the steps
//   that a one-decimal value reaches are used.
//
// The desk snaps what it reads to its nearest step. Bands are PEQ (the desk's
// parametric bell), assumed to follow the RBJ cookbook as the plugin's bells
// do (Q from the bandwidth between the half-gain points). The format is the
// community-documented one (Patrick-Gilles Maillot's unofficial X32/M32 OSC
// protocol, and files desks have written): a header line padded to 127
// characters, then one OSC parameter per line:
//
//   #4.0# "Room EQ" 4 0 65536 0 1
//   /bus/01/eq ON
//   /bus/01/eq/1 PEQ 95.5 -5.50 4.3
//
// The header's four masks say what the snippet recalls: parameter families
// (4: EQ), channels, aux-ins / FX returns / buses (bus n: bit 15 + n), and
// matrices / main LR (bit 6) / M/C (bit 7) / DCAs, as signed 32-bit numbers.

#include "roomeq/filters.h"
#include "roomeq/refit.h"

#include <string>
#include <vector>

namespace roomeq::x32
{
inline constexpr int numBands = 6;
inline constexpr int frequencySteps = 201;
inline constexpr int qSteps = 72;
inline constexpr double gainStepDb = 0.25;
inline constexpr double maxGainDb = 15.0;
inline constexpr double minQ = 0.3, maxQ = 10.0;
inline constexpr int headerWidth = 127;

enum class Strip
{
    bus = 0,       // mix bus 1-16
    matrix,        // matrix 1-6
    mainStereo,    // main LR
    mainMono       // main M/C
};

struct Destination
{
    Strip strip = Strip::bus;
    int number = 1;               // bus 1-16 or matrix 1-6 (ignored for the mains)
};

std::string destinationName (Destination d);   // "Bus 1", "Matrix 3", "Main LR", "Main M/C"
std::string stripPath (Destination d);         // "/bus/01", "/mtx/03", "/main/st", "/main/m"

// The desk's steps, and the values it ends up with for a value written.
double snapFrequency (double hz);
double snapGain (double db);
double snapQ (double q);                        // among the Qs a one-decimal value reaches

// Tokens as the desk writes them (independent of the locale).
std::string frequencyToken (double hz);         // "85.3", "4k53", "20k00"
std::string gainToken (double db);              // "+4.75", "-0.25", "+10.2"
std::string qToken (double q);                  // "2.0", "0.3", "10"

struct Fit
{
    std::vector<Band> bands;     // bells on the desk's steps, by frequency (at most six)
    int originalBands = 0;
    bool refitted = false;       // the correction needed fitting to six bells
    double maxErrorDb = 0.0;     // the desk's EQ vs the correction as it plays (refit grid)
    double rmsErrorDb = 0.0;
};

// The correction as played (gains already scaled by the amount), for the desk.
Fit fitForDesk (const std::vector<Band>& correction, double fs);

// The .snp text: header, the strip's EQ switched on, and all six bands (bands
// past the fit's are flat, so nothing already on the strip stays). `name` is
// shown on the desk: ASCII, at most 16 characters, no quotes.
std::string snippet (const Fit& fit, Destination d, const std::string& name);
} // namespace roomeq::x32
