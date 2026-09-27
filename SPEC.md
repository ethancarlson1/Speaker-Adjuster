# Adaptive Room EQ Plugin — Build Spec

Sep 27, 2026 · Ethan Carlson

## Overview

Build one VST3/AU plugin that measures a PA in the room, corrects it, and keeps the tonal balance consistent as volume drops and the room fills with people.

**Problems it solves**

- **Low-volume shows sound thin.** The PA and room barely change when turned down; hearing does. Equal-loudness contours (ISO 226) make lows and extreme highs fall off faster than mids.
- **Soundcheck doesn't match the show.** An audience absorbs mids and highs, cuts reverberant energy, and raises the noise floor.
- **Setup time is short and noisy.** Measurement has to work in a few minutes during load-in, not in a quiet empty room.

**Goals**

- Usable correction from 3–5 sweep captures in under 5 minutes, tolerant of load-in noise.
- Level-tracking loudness compensation calibrated to real SPL.
- Show-time tracking of the soundcheck-to-show change, applied only with the engineer's approval by default.
- Near-zero latency on the audio path; all heavy analysis runs off the audio thread.

**Non-goals (v1)**

- Feedback suppression and speech auto-leveling (separate project).
- Filling room nulls or correcting narrow peaks and dips.
- Multi-channel system alignment (delays, subs, arrays) beyond an optional temperature-based delay adjustment.

## Architecture

Three EQ stages sit on the audio path; one shared analysis engine feeds them from a separate thread.

```text
Input → [Measured correction] → [Voicing EQ] → [Loudness comp] → PA
              ↑        ↑                              ↑
   ┌──────────┴──┬─────┴────────────┬───────────────┴──┐
   │ Sweep capture │ Show-mode delta │  Level + noise    │  Analysis engine
   │ 3–5 positions │ coherence-gated │  SPL vs calib.    │  (off audio thread)
   └───────────────┴─────────────────┴───────────────────┘
        ↑ measurement mic (sidechain)   ↑ tap of plugin output (reference)
```

The measurement mic and a tap of the plugin's output feed the analysis engine. It updates the correction stage and the loudness stage, never the voicing EQ.

- **Stage order:** measured correction → voicing EQ → loudness compensation. Each stage has its own bypass.
- **Latency:** audio path under 1 ms using minimum-phase IIR filters. A linear-phase FIR option is a later add-on, reported to the host as latency.
- **Threading:** FFT analysis, averaging, and curve fitting run on a background thread. Filter coefficients cross to the audio thread lock-free and are smoothed to avoid zipper noise.
- **State:** measurements, calibration, and curves save with the session, plus export/import as venue presets.

## Phase 1: Measurement tool

Deliver a sweep-and-capture workflow that produces a trustworthy averaged room response in under 5 minutes.

**Capture**

- Exponential (log) sine sweep, 20 Hz–20 kHz, selectable 2 / 5 / 10 s. Deconvolve to an impulse response; this rejects harmonic distortion.
- 1–3 sweeps per position, averaged. The plugin plays the sweep through its own output and records the measurement mic via a sidechain input.
- Automatic loop-delay detection so captures line up.

**Positions**

- Target 3–5 positions across the audience area, at varied distances and off-axis. Prompt the user to avoid symmetric spots on the center line.
- Store each capture separately; the user can rename, exclude, or redo any one.

**Capture grading**

- Grade each capture on signal-to-noise ratio per band and on consistency between repeated sweeps.
- Show pass / marginal / redo, with the reason (e.g. "low-end noise too high below 80 Hz").

**Averaging and output**

- Power-average magnitude responses across positions, then apply fractional-octave smoothing (1/3 to 1/6 octave, user selectable).
- Display each position, the average, and the proposed target overlay.

**Quick mode**

- With a single capture, use heavier smoothing and cap correction at a few dB. Correction strength scales up as more good positions are added.

**Program-material fallback**

- When a sweep isn't possible, estimate the transfer function with dual-FFT against walk-in music or soundcheck, using the plugin input as reference. Same grading rules apply, weighted by coherence.

**Done when:** a 3-position capture in a noisy room produces a stable, repeatable average that matches a Smaart or REW measurement of the same system within a couple of dB after smoothing.

## Phase 2: Static correction and voicing EQ

Turn the averaged measurement into a conservative correction curve, and add a manual EQ layer that re-measuring never overwrites.

**Target curves**

- Presets: flat, gentle house curve (slight low-end lift, gentle top-end roll-off), and speech-focused. Users can draw and save their own.

**Correction fitting**

- Fit up to about 10 minimum-phase parametric bands to the difference between measurement and target.
- Correct broad trends only. Skip features narrower than a set bandwidth, since they are position-dependent.
- Asymmetric limits: cuts up to about 12 dB, boosts capped at a few dB (user-adjustable). Never boost into a detected null.
- Respect the system's usable range: no correction below the PA's measured low-frequency roll-off.

**Voicing EQ**

- Standard parametric EQ (bells, shelves, filters) after the correction stage. It is the engineer's taste layer.
- Re-running measurement updates only the correction stage.

**Display**

- Measured response, target, correction curve, voicing curve, and the combined predicted result on one graph.

**Done when:** re-measuring after applying correction shows the averaged response within a few dB of target across the corrected range, with no audible artifacts.

## Phase 3: Level calibration and loudness compensation

Keep perceived tonal balance steady when the PA runs below the level it was tuned at.

**Calibration**

- During setup, play pink noise at a known output level and enter (or measure with a calibrated mic) the SPL at the mix position.
- Store the relationship between plugin output level and SPL at that position.
- The user sets a **reference level**: the SPL at which the system was tuned and should sound "right".

**Level tracking**

- Estimate current SPL from the plugin's own output level against the calibration. Use slow, speech-friendly averaging (several seconds).
- Optional mic-based tracking, measured only during program to avoid counting crowd noise.

**Compensation curve**

- Compute the difference between the ISO 226 equal-loudness contours at current SPL and at reference level. Apply it as smooth low and high shelves.
- Controls: amount (0–100%), max low boost, max high boost, and response speed.
- No compensation at or above reference level.

**Safety**

- Hard ceiling on low-frequency boost to protect small systems from over-excursion.
- Optional high-pass that tracks the boost amount.

**Done when:** listening at 10–15 dB below reference keeps a similar perceived balance to the reference level, confirmed by A/B tests on speech and music.

## Phase 4: Show mode — delta tracking and noise compensation

Track how the room changes from soundcheck to show, and propose corrections for only that change.

**Reference snapshot**

- At the end of soundcheck, the user taps **Store reference**. The plugin saves the dual-FFT transfer function (plugin output vs. measurement mic) with the room empty.

**Live delta tracking**

- During the show, keep measuring the same transfer function against program material.
- Use only bands with high coherence; ignore bands the crowd or silence make unreliable.
- Average over minutes, not seconds. Compute the smoothed difference from the reference.

**Applying changes**

- **Suggest mode (default):** show the proposed correction as an overlay. The user applies it fully, partially (a slider), or ignores it.
- **Auto mode (optional):** apply changes slowly within a tight limit (a few dB), with a visible indicator.
- Changes feed the correction stage only; voicing EQ is untouched.

**Ambient noise compensation**

- Estimate crowd noise floor during pauses in program (quiet passages, gaps between talkers).
- When noise rises above its soundcheck level, nudge overall level and the 1–4 kHz intelligibility range up, within user limits.
- Coordinate with loudness compensation so the two don't stack beyond the set ceiling.

**Mic placement guidance**

- Recommend a measurement mic near FOH or overhead, away from direct crowd noise. A second mic position is optional for reliability.

**Done when:** in a real show, suggested changes are stable (no wandering) and moving toward what the engineer would do by ear.

## Phase 5: Prediction, learning, and environment

Get close to the show sound before the audience arrives, and improve with every gig.

**Predictive occupancy**

- A "room fullness" control (0–100%) applies the typical change an audience causes, starting from textbook absorption per person.
- Inputs: approximate room size and expected headcount. Output: a proposed delta curve the user can preview during soundcheck.

**Cross-show learning**

- After each show, log the measured soundcheck-to-show delta with basic metadata: room type, approximate size, headcount, PA type, and notes.
- Replace the textbook prediction with an average of similar logged rooms once there are enough entries.
- Store logs as plain JSON files so they can be backed up, inspected, and shared.

**Environmental inputs (optional)**

- Temperature and humidity, entered manually or read from a sensor.
- Adjust high-frequency air-absorption compensation over the listener distance.
- Report suggested delay changes for delay speakers as temperature shifts the speed of sound.

**Done when:** across several logged shows, the predicted delta gets measurably closer to the actual measured delta than the textbook default.

## Tech stack, parameters, and testing

Build in C++ with JUCE, test DSP offline in Python first, and validate against Smaart or REW.

**Stack**

- JUCE (C++17 or later), CMake, VST3 and AU targets, built cross-platform: VST3 on macOS and Windows, AU on macOS only; standalone app for measurement sessions on both. AAX later, if needed (requires Avid's developer program).
- Plugin needs a stereo main bus plus a mono sidechain input for the measurement mic.
- Python (NumPy/SciPy) prototypes for sweep deconvolution, averaging, and curve fitting, with the C++ port checked against them.

**Exposed parameters**

| Stage | Parameters |
| --- | --- |
| Measurement | sweep length, sweeps per position, smoothing (octave fraction), target curve |
| Correction | amount (%), max cut (dB), max boost (dB), frequency range, bypass |
| Voicing EQ | bands: type, frequency, gain, Q; bypass |
| Loudness | reference SPL, amount (%), max low boost, max high boost, speed, bypass |
| Show mode | suggest / auto, apply amount (%), averaging time, auto limit (dB) |
| Noise comp | amount (%), max level lift (dB), max 1–4 kHz lift (dB) |
| Environment | temperature, humidity, listener distance |

**Testing**

- **Offline room simulator:** convolve test signals with measured or generated impulse responses (pyroomacoustics) and add noise, to test capture grading and fitting without a PA.
- **Occupancy simulator:** apply known absorption changes to impulse responses to check that delta tracking finds them.
- **Reference comparison:** measure the same system with Smaart or REW and compare within 1/6-octave smoothing.
- **Real-room checks:** empty vs. full measurements saved from real gigs become a regression set.
- Unit tests for every DSP block; CPU and latency checks on each build.

## Starting prompt for Claude Code

Save this file in a new repo as `SPEC.md`, and start Claude Code with the prompt below.

```text
Read SPEC.md. We're building the adaptive room EQ plugin it describes, one phase at a time. Start with Phase 1 only.

1. Set up a cross-platform JUCE + CMake project that builds on macOS and Windows, with VST3, AU (macOS only), and Standalone targets, a stereo main bus, and a mono sidechain input for the measurement mic.
2. Before any C++, write a Python prototype in /prototype: log sweep generation, deconvolution to an impulse response, per-band SNR grading, multi-position power averaging, and fractional-octave smoothing. Include an offline room simulator (pyroomacoustics + added noise) and tests.
3. Show me plots from the prototype and wait for my feedback before porting to C++.
4. Then port to C++, keeping analysis off the audio thread, and add a basic UI: capture list with grades, and a response graph (each position, the average, the target).

Ask me before making decisions the spec leaves open. Commit after each working step.
```

After Phase 1 works on a real PA, start a new session for each later phase with the same pattern: prototype, review, port.
