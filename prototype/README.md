# DSP prototype

A Python (NumPy/SciPy) reference implementation of the measurement engine (Phase 1), the correction fit (Phase 2) and loudness compensation (Phase 3), plus an offline room simulator for testing them without a PA. The C++ port is checked against this.

```sh
pip install -r requirements.txt
pytest                      # ~1 min
python make_plots.py        # Phase 1 review plots (plots/01-06)
python make_phase2_plots.py # Phase 2 review plots (plots/07-11)
python make_phase3_plots.py # Phase 3 review plots (plots/12-14)
```

**C++ cross-check:** `tests/test_cpp_port.py` runs the C++ port (`roomeq_cli`, built from `src/roomeq`) on the same simulated recordings. It requires every delay, band SNR/level/spread, grade, reason string, offset and curve to match this prototype within 1e-6 dB. Point `ROOMEQ_CLI` at the built binary to run it (CI always does); without it those tests are skipped.

## Modules (`roomeq/`)

| Module | What it does |
| --- | --- |
| `sweep.py` | Log sweep generation, regularised deconvolution to an impulse response, arrival (loop delay) detection, sub-sample alignment |
| `spectrum.py` | Analysis window, log frequency grid, 1/N-octave power smoothing, octave bands |
| `grading.py` | Per-octave SNR + repeat consistency → pass / marginal / redo, with reasons |
| `averaging.py` | Weighted multi-position power average, level alignment, PA usable range, quick-mode policy |
| `dualfft.py` | Program-material fallback: H1 transfer function, coherence, noise estimate |
| `noise.py` | Pink noise measurement signal (analysed with the dual-FFT) |
| `capture.py` | Ties it together: one position's recordings → graded `Capture` |
| `filters.py` | RBJ biquads (bell, shelves, high/low-pass) and their exact digital responses |
| `targets.py` | Target curves (flat, house, speech, user-drawn) as points with PCHIP interpolation in log frequency |
| `correction.py` | Phase 2: fit range, null detection, and the bounded greedy + Levenberg–Marquardt band fit |
| `iso226.py` | ISO 226:2003 equal-loudness contours |
| `loudness.py` | Phase 3: C-weighting, calibration, level tracking, and the contour-difference compensation as shelves with limits and a tracking high-pass |
| `roomsim.py` | Simulator: PA model with mild distortion → pyroomacoustics room → mic, plus load-in noise (pink, rumble, crowd, bangs) and synthetic walk-in music |

## How a capture is analysed

1. **Sweep:** Farina exponential sweep, 20 Hz–20 kHz, 2 / 5 / 10 s, −12 dBFS peak, with fades sized in octaves. Each recording is 0.25 s pre-roll + sweep + 2 s tail.
2. **Deconvolve:** `conj(X) / (|X|² + ε(f))`. ε is tiny in-band, so the response there is exact (< 0.01 dB), and large outside, band-limiting the IR over a 1-octave transition. Harmonic distortion lands before the linear IR and is windowed out.
3. **Loop delay:** the peak of |h| after the sweep start (latency + flight time, searched up to 1 s). Repeats are aligned to the sample by their own peaks, then to a fraction of a sample by cross-correlation, and averaged coherently (~3 dB SNR per doubling).
4. **Window:** 50 ms before the peak (keeps the band-limit's pre-ringing) to 500 ms after, with half-Hann tapers. This is a fixed REW-style window.
5. **Noise:** the same window applied to the late IR tail, which is still valid at every frequency. This captures the noise that was present *during* the sweep.
6. **Grade** (per octave, 31.5 Hz–16 kHz):
   - **SNR:** pass ≥ 20 dB, marginal ≥ 10 dB, otherwise redo.
   - **Repeat consistency:** checked per 1/3 octave. Only spread beyond what the measured noise explains counts. Pass ≤ 1 dB, marginal ≤ 3 dB.
   - **Out of range:** bands more than 10 dB below the 250 Hz–4 kHz level at either end are not graded.
7. **Average:** a power average of |H|² across positions:
   - Levels are aligned first (250 Hz–4 kHz), so near positions don't dominate.
   - A capture's redo bands are dropped from the average, but its good bands still count.
   - After averaging, 1/N-octave smoothing is applied.

**Program-material fallback:** dual-FFT H1 with a 1.4 s frame (at 48 kHz), which holds the low-frequency reverb. SNR = coherent/incoherent power × effective averages, graded with the same thresholds. Coherence decides the grade but does **not** weight the magnitude: coherence peaks where the room response peaks, and weighting by it read 1–2 dB high.

## How the correction is fitted (Phase 2)

1. **Inputs:** the averaged response at the session's smoothing, its 1-octave trend, and a 1/6-octave version, on a 24-points-per-octave grid.
2. **Target:** a preset or user-drawn curve, placed on the average by its 250 Hz–4 kHz mean.
3. **Fit range:** the PA's −6 dB points on the 1-octave trend, intersected with the user's frequency range. Nothing is boosted outside it.
4. **Nulls:** the average dips more than 6 dB below its 1-octave trend, or the positions (1/3 octave, level-aligned) disagree by more than 6 dB; widened by 1/6 octave. The correction may cut there but never boost.
5. **Wanted correction:** target − average, clipped to −12 / +3 dB (0 dB boost in nulls, 0 outside the range).
6. **Fit:** up to 10 bands, added greedily. Each step tries a bell on the largest-area residual and low/high shelves at 1/3-octave steps, refines each candidate alone, and keeps the best; then all bands are refined together (bounded Levenberg–Marquardt, limits enforced as penalties). A band must cut the cost by 2% and end up ≥ 0.5 dB.
   - Widths: boosts ≥ 1 octave, cuts ≥ 1/3 octave below ~300 Hz and ≥ 2/3 octave above, nothing wider than 3 octaves; shelves use S = 1.
7. **Quick mode:** the limits are raised to cap/strength, then every gain is scaled by the strength, so with one good position the applied correction is half strength and within ±3 dB.

Re-measuring the simulated club through the fitted filters lands within 0.3–0.4 dB RMS of each preset target (worst 1.3 dB) over the corrected range.

## How loudness compensation works (Phase 3)

1. **Calibration:** pink noise plays on both speakers. The C-weighted level at the loudness stage's input (after correction and voicing) is paired with the SPL at the mix position: typed in from a meter (dB C, slow) or read by a calibrated mic. The mic's own level is noted at the same time, so an uncalibrated measurement mic can track SPL later.
2. **Tracking:** 400 ms C-weighted momentary levels.
   - The estimate rises with a 1 s time constant, so the boost backs off quickly when it gets louder. It falls with the **speed** setting (5 s), so the boost grows slowly when it gets quieter.
   - Silence, and anything more than 20 dB below the estimate, counts as a pause and holds the estimate. A drop that lasts longer than 8 s is real and is followed.
   - Mic tracking only updates while the plugin's output shows program playing.
3. **Target:** the ISO 226 contour difference between the current SPL and the reference, `[Lp(f, now) − now] − [Lp(f, ref) − ref]`, zero at or above the reference.
4. **Shelves:** that difference keeps its shape as the level drops and just scales, so a low shelf and a high shelf are fitted once per reference level (frequency and Q). A table then gives their gains for every dB below the reference. They stay within 0.8 dB of the ISO curve from 30 Hz to 16 kHz for drops up to 20 dB.
5. **Limits:** amount, max low boost (8 dB), max high boost (4 dB), and a fixed 12 dB low-boost ceiling.
6. **Protective high-pass (optional):** 24 dB/octave at the PA's measured roll-off, rising up to half an octave as the low boost reaches its maximum.
