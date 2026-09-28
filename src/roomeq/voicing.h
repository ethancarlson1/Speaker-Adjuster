#pragma once

// Voicing EQ bands: the engineer's taste layer after the correction.
// A band is one or two filter sections (24 dB/octave filters are two
// Butterworth sections); the audio path and the graph build them the same way.

#include "roomeq/filters.h"

#include <array>

namespace roomeq
{
enum class VoicingType
{
    bell = 0,
    lowShelf,
    highShelf,
    highPass12,
    highPass24,
    lowPass12,
    lowPass24
};

inline constexpr int numVoicingTypes = 7;
inline constexpr int numVoicingBands = 8;

struct VoicingBand
{
    bool on = false;
    VoicingType type = VoicingType::bell;
    double freq = 1000.0;
    double gainDb = 0.0;     // bells and shelves
    double q = 1.0;          // bells, shelves and 12 dB/octave filters

    bool operator== (const VoicingBand& o) const;   // exact
    bool operator!= (const VoicingBand& o) const { return ! (*this == o); }
};

// Up to two filter sections, no allocation (used on the audio thread).
struct Sections
{
    std::array<Band, 2> bands {};
    int count = 0;
};

Sections voicingSections (const VoicingBand& band);

bool hasGain (VoicingType type);   // bell and shelves
bool hasQ (VoicingType type);      // not the 24 dB/octave filters

// Default layout of the eight bands (all off): 35 Hz high-pass, low shelf,
// four bells, high shelf, 18 kHz low-pass.
VoicingBand defaultVoicingBand (int index);
} // namespace roomeq
