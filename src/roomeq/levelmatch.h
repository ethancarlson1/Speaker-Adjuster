#pragma once

// Output level matching: the make-up gain that keeps an EQ from changing the
// loudness of typical program. The EQ's power gain is taken on pink noise
// (equal energy per octave, 20 Hz-20 kHz), K-weighted as loudness meters do
// (ITU-R BS.1770, LUFS), and inverted.
//
// Nothing here allocates after prepare(), so the audio thread can run it.

#include "roomeq/filters.h"

#include <array>
#include <vector>

namespace roomeq
{
// ITU-R BS.1770 K-weighting as two RBJ sections (the "pre-filter" shelf and the RLB high-pass).
std::array<Band, 2> kWeightingBands();

class LevelMatch
{
public:
    static constexpr int pointsPerOctave = 24;
    static constexpr int maxPoints = 256;          // 20 Hz-20 kHz at 24 per octave is 240
    static constexpr double maxGainDb = 12.0;      // make-up never exceeds this either way

    using Power = std::array<double, maxPoints>;   // an EQ's power gain per grid point

    void prepare (double fs) noexcept;
    int size() const noexcept { return count; }

    // Building a stage's power curve: start flat, multiply in each band.
    static void flat (Power& p) noexcept { p.fill (1.0); }
    void multiply (Power& p, const Band& band) const noexcept;

    // Make-up gain (dB) for stages in series, clamped to +-maxGainDb.
    double makeupDb (const Power& a, const Power& b) const noexcept;

    // Convenience (allocates): make-up for a list of bands.
    double makeupDb (const std::vector<Band>& bands) const;

private:
    Power cos1 {}, cos2 {}, weight {};
    double weightSum = 1.0;
    double sampleRate = 48000.0;
    int count = 0;
};
} // namespace roomeq
