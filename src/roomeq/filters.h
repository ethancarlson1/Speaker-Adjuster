#pragma once

// Minimum-phase EQ bands (RBJ cookbook biquads) and their exact digital
// responses. Used by the fitted correction and the voicing EQ.
// Port of prototype/roomeq/filters.py.

#include <vector>

namespace roomeq
{
enum class BandKind
{
    bell = 0,
    lowShelf,
    highShelf,
    highPass,
    lowPass
};

const char* bandKindName (BandKind kind);    // "bell", "low_shelf", ... (as in the prototype)

inline constexpr double shelfQ = 0.70710678118654752440;   // RBJ shelf slope S = 1

struct Band
{
    BandKind kind = BandKind::bell;
    double freq = 1000.0;      // Hz: centre (bell), half-gain point (shelf), -3 dB point (pass)
    double gainDb = 0.0;       // ignored by the pass filters
    double q = shelfQ;
    bool enabled = true;

    Band scaled (double amount) const;   // same band, gain scaled
    bool operator== (const Band& o) const;     // exact (defined out of line: -Wfloat-equal)
    bool operator!= (const Band& o) const { return ! (*this == o); }
};

// Normalised coefficients (a0 = 1).
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

double qForBandwidth (double octaves);   // bell Q for a bandwidth in octaves (half-gain points)
double bandwidthForQ (double q);

Biquad designBiquad (const Band& band, double fs);
double biquadDb (const Biquad& c, double f, double fs);

std::vector<double> bandDb (const Band& band, const std::vector<double>& freqs, double fs);
std::vector<double> responseDb (const std::vector<Band>& bands, const std::vector<double>& freqs, double fs);
} // namespace roomeq
