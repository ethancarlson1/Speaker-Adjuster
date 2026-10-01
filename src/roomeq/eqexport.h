#pragma once

// The EQ as a file another tool can read, so the plugin needn't stay in the
// signal path: the correction (as it plays, at the current Amount), the
// voicing bands that are on, the output level-match gain, and the zone's delay
// and polarity. Versioned, and independent of the plugin's session state.
//
// Three forms of the same data:
// - JSON: {"format": "adaptive-room-eq.eq", "version": 1, ...}
// - CSV: one filter (or setting) per row, after #-comment lines;
// - text: laid out for typing into a console.
//
// Filters are RBJ Audio EQ Cookbook biquads. A bell's Q sets its bandwidth
// between the points at half its gain in dB (bandwidth_octaves gives the same
// width); a shelf's frequency is its half-gain point and Q 0.707 is slope 1;
// high- and low-pass frequencies are -3 dB points (24 dB/octave: two
// Butterworth sections).

#include "roomeq/filters.h"
#include "roomeq/voicing.h"

#include <string>
#include <vector>

namespace roomeq
{
inline constexpr const char* eqExportFormat = "adaptive-room-eq.eq";
inline constexpr int eqExportVersion = 1;

struct EqExport
{
    std::string generator;                 // "Adaptive Room EQ 0.1.0"
    std::string created;                   // ISO 8601
    std::string source;                    // "Mains (PA)": the zone and track
    double sampleRate = 48000.0;

    std::string correctionStatus = "none"; // "applied", "proposed" (nothing applied yet) or "none"
    bool correctionOn = true;
    double amount = 1.0;                   // already in the exported gains
    std::vector<Band> correction;          // as they play: gains scaled by the amount

    bool voicingOn = true;
    std::vector<VoicingBand> voicing;      // the bands that are on

    double outputGainDb = 0.0;             // the level-match make-up (0 when off)
    double delayMs = 0.0;
    bool polarityInverted = false;
};

std::string eqExportJson (const EqExport& e);
std::string eqExportCsv (const EqExport& e);
std::string eqExportText (const EqExport& e);
} // namespace roomeq
