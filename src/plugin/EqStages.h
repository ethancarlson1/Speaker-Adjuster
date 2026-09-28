#pragma once

// The EQ on the audio path: measured correction -> voicing EQ, both stereo
// with the same settings on both sides. JUCE-free, so it's unit tested.
//
// Each filter section is a trapezoidal state-variable filter (Simper). Its
// magnitude response is identical to the RBJ biquad of the same band (what
// the fit and the graph use), and its coefficients can glide from one
// setting to another per sample without clicks: changing a band, applying a
// new correction, A/B and bypass all glide over ~20 ms.

#include "plugin/LatestValue.h"
#include "roomeq/filters.h"
#include "roomeq/voicing.h"

#include <array>

struct SvfCoeffs
{
    double g = 0.1, k = 1.0;             // tan(pi f / fs), damping
    double m0 = 1.0, m1 = 0.0, m2 = 0.0; // output mix; (1, 0, 0) passes the input unchanged
};

SvfCoeffs designSvf (const roomeq::Band& band, double fs) noexcept;

class EqChain
{
public:
    static constexpr int maxSections = 16;
    static constexpr int maxChannels = 2;

    void prepare (double sampleRate, double glideSeconds = 0.02) noexcept;
    void reset() noexcept;                                  // clears the filter state
    void setTargets (const SvfCoeffs* coeffs, int count) noexcept;   // the rest glide to pass-through
    void jumpToTargets() noexcept;
    void process (float* const* channels, int numChannels, int numSamples) noexcept;
    bool isGliding() const noexcept;

private:
    struct Section
    {
        SvfCoeffs cur, tgt;
        double a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double ic1[maxChannels] {}, ic2[maxChannels] {};
        bool gliding = false;
        bool passThrough = true;          // cur is pass-through and not gliding: skipped
    };

    static void updateDerived (Section& s) noexcept;
    void updateActiveCount() noexcept;

    std::array<Section, maxSections> sections {};
    int numActive = 0;
    double glide = 0.001;
};

// A fitted correction as the audio thread receives it (trivially copyable).
struct CorrectionSet
{
    static constexpr int maxBands = 12;
    std::array<roomeq::Band, maxBands> bands {};
    int count = 0;
};

// Everything the audio thread reads from the parameters, once per block.
struct EqSettings
{
    bool correctionOn = true;
    double amount = 1.0;                  // 0..1, scales the correction's gains
    bool voicingOn = true;
    std::array<roomeq::VoicingBand, roomeq::numVoicingBands> voicing {};
};

class EqStages
{
public:
    // Audio stopped: set everything without gliding.
    void prepare (double sampleRate, const EqSettings& settings) noexcept;

    // Message thread (lock-free): the correction to play.
    void setCorrection (const CorrectionSet& set) noexcept { pending.write (set); }

    // Audio thread.
    void process (float* const* channels, int numChannels, int numSamples, const EqSettings& settings) noexcept;

    // Audio thread, for blocks where nothing plays through the EQ (the
    // standalone app between measurements): keep up with the settings and
    // settle at once, so a later measurement through the EQ starts settled.
    void skip (const EqSettings& settings) noexcept;

private:
    void syncSettings (const EqSettings& settings) noexcept;
    void updateCorrectionTargets() noexcept;
    void updateVoicingTargets() noexcept;

    LatestValue<CorrectionSet> pending;
    CorrectionSet correction;
    EqSettings current;
    double fs = 48000.0;
    EqChain correctionChain, voicingChain;
};
