# Phase 1 DSP prototype

A Python (NumPy/SciPy) reference implementation of the Phase 1 measurement engine, plus an offline room simulator for testing it without a PA. The C++ port is checked against this.

```sh
pip install -r requirements.txt
pytest                      # ~25 s
python make_plots.py        # regenerates the review plots in plots/
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
