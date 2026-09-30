"""ISO 226:2003 normal equal-loudness-level contours.

Revision: ISO 226:2003 (second edition). The current edition is ISO
226:2023; its parameters aren't encoded here.

Lp(f, Ln) is the sound pressure level at frequency f that sounds as loud as
a 1 kHz tone at Ln dB (Ln phon), for pure tones heard frontally in a free
field by otologically normal listeners aged 18-25.

How it's encoded:
- The three parameters of Table 1 (alpha_f, L_U, T_f) at the standard's 29
  frequencies, 20 Hz-12.5 kHz, typed from the table.
- Lp is the standard's formula (clause 4.1) evaluated at those frequencies,
  so the contours are exactly the standard's at its frequencies. The tests
  pin the 40 and 80 phon contours to two decimals; they agree with the
  widely used public implementations of the same formula.

Approximations and assumptions:
- Between the table frequencies the contour is interpolated linearly in log
  frequency; below 20 Hz and above 12.5 kHz it's held at the end values (the
  standard stops there).
- The formula is specified for 20-90 phon (20-80 phon from 5 kHz up).
  Outside that it's used as is, which keeps loudness compensation smooth;
  the compensation clamps anything below 20 phon to 20.
- At 1 kHz the formula gives Ln to within 0.02 dB, not exactly; the
  relative contours subtract their own 1 kHz value, so that doesn't matter.
"""

from __future__ import annotations

import numpy as np

# ISO 226:2003 Table 1.
FREQS = np.array([20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
                  1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500], dtype=float)
ALPHA_F = np.array([0.532, 0.506, 0.480, 0.455, 0.432, 0.409, 0.387, 0.367, 0.349, 0.330, 0.315, 0.301,
                    0.288, 0.276, 0.267, 0.259, 0.253, 0.250, 0.246, 0.244, 0.243, 0.243, 0.243, 0.242,
                    0.242, 0.245, 0.254, 0.271, 0.301])
L_U = np.array([-31.6, -27.2, -23.0, -19.1, -15.9, -13.0, -10.3, -8.1, -6.2, -4.5, -3.1, -2.0, -1.1, -0.4,
                0.0, 0.3, 0.5, 0.0, -2.7, -4.1, -1.0, 1.7, 2.5, 1.2, -2.1, -7.1, -11.2, -10.7, -3.1])
T_F = np.array([78.5, 68.7, 59.5, 51.1, 44.0, 37.5, 31.5, 26.5, 22.1, 17.9, 14.4, 11.4, 8.6, 6.2, 4.4,
                3.0, 2.2, 2.4, 3.5, 1.7, -1.3, -4.2, -6.0, -5.4, -1.5, 6.0, 12.6, 13.9, 12.3])


def contour_at_table(phon: float) -> np.ndarray:
    """Lp (dB SPL) at the 29 table frequencies for a loudness level in phon."""
    af = 4.47e-3 * (10 ** (0.025 * phon) - 1.15) + (0.4 * 10 ** ((T_F + L_U) / 10 - 9)) ** ALPHA_F
    return 10 / ALPHA_F * np.log10(af) - L_U + 94


def contour(freqs: np.ndarray, phon: float) -> np.ndarray:
    """Lp (dB SPL) at arbitrary frequencies: linear in log frequency between table points, held outside."""
    x = np.log2(np.clip(np.asarray(freqs, dtype=float), FREQS[0], FREQS[-1]))
    return np.interp(x, np.log2(FREQS), contour_at_table(phon))


def relative_contour(freqs: np.ndarray, phon: float) -> np.ndarray:
    """Contour minus its 1 kHz value: how much more level each frequency needs than 1 kHz."""
    return contour(freqs, phon) - contour(np.array([1000.0]), phon)[0]
