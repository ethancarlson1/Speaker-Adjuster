#pragma once

// ISO 226:2003 (second edition) normal equal-loudness-level contours. Port of
// prototype/roomeq/iso226.py, which documents the encoding and assumptions.
//
// contour(f, phon) is the SPL at f that sounds as loud as 1 kHz at `phon` dB.
// It's the standard's formula (clause 4.1) with its Table 1 parameters at the
// 29 table frequencies (20 Hz-12.5 kHz); between them the contour is linear in
// log frequency, and it is held beyond the ends. The formula is specified for
// 20-90 phon (20-80 from 5 kHz); outside that it's used as is.

#include <vector>

namespace roomeq
{
std::vector<double> iso226ContourAtTable (double phon);   // at the 29 table frequencies
std::vector<double> iso226Contour (const std::vector<double>& freqs, double phon);
std::vector<double> iso226RelativeContour (const std::vector<double>& freqs, double phon);   // minus its 1 kHz value
const std::vector<double>& iso226Freqs();
} // namespace roomeq
