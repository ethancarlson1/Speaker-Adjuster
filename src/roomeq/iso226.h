#pragma once

// ISO 226:2003 equal-loudness contours. Port of prototype/roomeq/iso226.py.
//
// contour(f, phon) is the SPL at f that sounds as loud as 1 kHz at `phon` dB.
// The standard tabulates 29 frequencies (20 Hz-12.5 kHz); between them the
// contour is linear in log frequency, and it is held beyond the ends.

#include <vector>

namespace roomeq
{
std::vector<double> iso226ContourAtTable (double phon);   // at the 29 table frequencies
std::vector<double> iso226Contour (const std::vector<double>& freqs, double phon);
std::vector<double> iso226RelativeContour (const std::vector<double>& freqs, double phon);   // minus its 1 kHz value
const std::vector<double>& iso226Freqs();
} // namespace roomeq
