#pragma once

// Pink noise measurement signal. Port of prototype/roomeq/noise.py (same
// algorithm; the random sequence differs from numpy's).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace roomeq
{
struct NoiseConfig
{
    double fs = 48000.0;
    double duration = 20.0;       // seconds (10 / 20 / 30)
    double f1 = 20.0;
    double f2 = 20000.0;
    double levelDbfs = -12.0;     // peak level, like the sweep
    double fade = 0.05;           // seconds, raised-cosine in and out
    double tail = 1.0;            // seconds recorded after the noise stops
    std::uint32_t seed = 1;

    std::size_t nNoise() const { return static_cast<std::size_t> (std::llround (duration * fs)); }
    std::size_t nTail() const  { return static_cast<std::size_t> (std::llround (tail * fs)); }
};

// Band-limited pink noise (-3 dB/octave, f1..f2), peaks clipped at 3 sigma
// (crest factor ~9.5 dB), faded, and peak-normalised to levelDbfs.
std::vector<double> generatePinkNoise (const NoiseConfig& cfg);
} // namespace roomeq
