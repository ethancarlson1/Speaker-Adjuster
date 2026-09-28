# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phases 1 and 2 are implemented.
- **Measure:** positions measured with log sweeps or pink noise (or from program material), each capture graded, and every position, the power average and the target shown.
- **Correct:** a conservative minimum-phase correction fitted to the average, applied on your say-so and checked by measuring through it.
- **Voicing:** an 8-band voicing EQ on top of the correction.

So far all of this has only been checked against simulated rooms. The next step is a real PA, compared with Smaart or REW.

## Measuring a room (Measure tab)

1. Route the measurement mic to the plugin's **sidechain input**. In the standalone app, pick it in **Mic input** instead.
2. Pick the speaker to measure. In the plugin that's **Speaker** (Left or Right), and the other side stays silent. In the standalone app it's **Speaker output**, any single interface output. The correction is applied to both sides.
3. Pick the **Signal**:
   - **Sweep** (default): a log sine sweep. Each position plays 1–3 sweeps (2, 5 or 10 s) and averages them. It is quick, has the best signal-to-noise ratio, and keeps harmonic distortion out of the result.
   - **Pink noise**: 10, 20 or 30 s of pink noise, analysed against the exact noise that was played (dual-FFT). It needs longer for the same accuracy and doesn't separate out distortion, but a single bang or cough averages out instead of spoiling the capture. 20 s is a good default.

   Both match within about 1 dB above 60 Hz in the simulated rooms.
4. Set **Level** so the mic meter peaks well below 0 dBFS. The default is −12 dBFS. It's a peak level for both signals, so pink noise (about 9.5 dB crest factor) plays about 6–7 dB quieter on average than a sweep at the same setting.
5. Press **Measure position** at 3–5 spots across the audience area, at different distances and off-axis. Avoid symmetric spots on the centre line.
6. Each capture is graded **pass / marginal / redo** with the reason (e.g. "low-end noise too high below 180 Hz"). You can rename a capture (double-click), take it out of the average, redo it, or delete it.
7. The graph shows each position (level-aligned), the power average, and the target over the corrected range. With only 1–2 good positions, quick mode smooths more heavily and limits the correction (below).

Measurements always play straight to the speaker, bypassing the correction and voicing EQ, so the fit always sees the PA's own response.

**Measure from music (30 s)** estimates the response from walk-in music or soundcheck when a sweep isn't possible. It's a dual-FFT against the plugin's output, so it also measures the PA's own response while a correction is on. Both speakers play during it.

## Correcting (Correct tab)

1. Pick a **Target**:
   - **Flat**.
   - **House:** +4 dB below ~80 Hz easing to 0 by 250 Hz, and −1 dB/octave above 2 kHz (−3 dB at 16 kHz).
   - **Speech:** −6 dB at 75 Hz, +2 dB at 2–4 kHz, and −3 dB at 16 kHz.
   - **Custom:** drag its points on the graph; double-click to add or remove one. The **Targets…** menu saves it as a file, loads saved ones, or starts one from a preset.

   The target is placed on the average by its 250 Hz–4 kHz level, so the correction reshapes the ends rather than moving the overall level.
2. The proposed correction updates as you measure. The graph shows it:
   - dashed in the EQ strip until applied;
   - as the **Predicted** curve in the top panel.

   It's deliberately conservative:
   - Up to 10 bands, cuts up to **Max cut** (12 dB) and boosts up to **Max boost** (3 dB).
   - Boosts are at least 1 octave wide. Cuts are at least 1/3 octave wide below ~300 Hz and 2/3 octave above.
   - Nothing is corrected outside the PA's −6 dB points or the **Correct from / up to** range.
   - Nothing is ever boosted into a null: a dip more than 6 dB below the trend, or a range where the positions disagree by more than 6 dB. Those are cut if needed but never filled.
3. **Apply correction** puts the proposal on the audio path, gliding in over ~20 ms, and keeps the one it replaces:
   - **Hear previous** switches to the previous correction while it's on.
   - **Undo** swaps back (press it again to swap forward).
   - **Amount** scales the applied correction, and **Correction on** bypasses it.
4. **Verify: measure through the EQ** measures a position with the signal played through the correction and voicing, like the audience hears it. Verify captures (V1, V2…) never change the proposal. Their average is the violet **Verified** curve, and the panel shows how far it is from the target.

**Quick mode:** with one good position the applied correction is half strength and within ±3 dB; with two, 75% and ±6 dB. Full strength needs three.

## Voicing EQ (Voicing tab)

Eight bands (bell, low/high shelf, 12 or 24 dB/octave high/low-pass) after the correction: the engineer's taste layer. Measuring never changes it. Edit a band in the tab, or on the graph:
- drag its numbered handle in the EQ strip (sideways for frequency, up and down for gain);
- use the mouse wheel for Q;
- double-click to switch it on or off.

All voicing and correction settings are automatable parameters. Measurements, the applied and previous corrections, and the custom target are saved with the host session.

## Trying it out

### 1. Get a build

Download from the latest green CI run (Actions → CI → the run → **Artifacts**, requires a GitHub login), or build it yourself (see [Building](#building)).

- **macOS (`AdaptiveRoomEQ-macOS`):** unzip the artifact, then unzip the bundles inside it. Copy them into place:
  - `Adaptive Room EQ.vst3` → `~/Library/Audio/Plug-Ins/VST3/`
  - `Adaptive Room EQ.component` → `~/Library/Audio/Plug-Ins/Components/`
  - `Adaptive Room EQ.app` → wherever you like

  The builds aren't notarized, so clear macOS's download quarantine and refresh the AU cache:

  ```sh
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Adaptive Room EQ.vst3" \
      ~/Library/Audio/Plug-Ins/Components/"Adaptive Room EQ.component" "/path/to/Adaptive Room EQ.app"
  killall -9 AudioComponentRegistrar
  auval -v aufx Areq Ardv        # should end with "AU VALIDATION SUCCEEDED"
  ```

- **Windows (`AdaptiveRoomEQ-Windows`):**
  - Copy the `Adaptive Room EQ.vst3` folder to `C:\Program Files\Common Files\VST3\`.
  - `Adaptive Room EQ.exe` is the standalone app. It isn't signed, so SmartScreen may ask you to confirm the first time you run it.

### 2. Loopback check (no PA needed, 5 minutes)

Patch an interface output into the mic input with a cable:
- **Plugin:** the output the test signal plays on, into the input you route to the sidechain.
- **Standalone app:** the **Speaker output**, into the **Mic input**.

Press **Measure position**. You should get **PASS** in every band. Expect a nearly flat curve (the interface's own response), and a delay equal to your interface's round-trip latency (a few ms). If that works, recording, deconvolution and timing are all behaving.

### 3. Desk check

Put a small speaker and the measurement mic about 1 m apart and measure. Then check two things:
- **Delay:** it should be the loopback delay plus about 2.9 ms per metre.
- **Repeatability:** measuring the same spot twice should give near-identical curves.

### 4. In a DAW

Any host that can feed a live input to a plugin sidechain works. Reaper is the most flexible:
1. Put the plugin on the track (or master) that feeds the PA. Set that track to 4 channels.
2. Record-arm the mic's input track with monitoring on.
3. Add a send from the mic track (1/2 → 3/4) to the plugin track. Reaper maps channels 3/4 to the plugin's sidechain.

In Logic, choose the mic's input or track from the AU's **Side Chain** menu. The plugin's header shows "Mic not connected" until the sidechain is live.

### 5. On a real PA

Phase 1's "done when" test:
- Start with **Level** around −30 dBFS and the amps turned down. Bring it up until captures grade PASS, with the mic meter well below 0 dBFS.
- Measure 3 positions and look at the average.
- Then measure the same spots with REW or Smaart (same mic, 1/6-octave smoothing, RMS/power average with SPL alignment) and compare. They should agree within a couple of dB.
- Measure a second time with the plugin to check it's repeatable.

Phase 2's "done when" test:
- Measure 3–5 positions, pick a target, and press **Apply correction**.
- Press **Verify** at two or three of the same spots. The violet Verified curve should sit within a few dB of the target across the corrected range. The Correct tab shows the RMS difference; the simulated room lands under 1 dB.
- Play music and flip **Hear previous** / **Correction on** to listen for artifacts: there should be none, just the tonal change.

## Layout

| Path | What |
| --- | --- |
| `src/roomeq/` | Analysis core: plain C++17, no JUCE. A port of the Python prototype, using pocketfft |
| `src/plugin/` | JUCE plugin: real-time sweep/noise player and recorder, the correction and voicing EQ (state-variable filters that glide), background analysis and fitting, UI |
| `prototype/` | Python (NumPy/SciPy) reference implementation, room simulator, tests, review plots |
| `tests/cpp/` | C++ unit tests (doctest) for the core, the real-time recorder and the EQ |
| `tools/roomeq_cli.cpp` | Runs the C++ core on raw recordings and prints JSON. Used to cross-check against Python |
| `tools/plugin_harness.cpp` | Headless end-to-end run of the real processor against a simulated room; renders the UI to PNG |
| `.github/workflows/ci.yml` | Builds on macOS, Windows and Linux; runs every test layer and pluginval |

## Building

Requirements: CMake 3.22+, a C++17 compiler (Xcode on macOS, Visual Studio 2019+ on Windows). The first configure downloads JUCE 8.0.15, pocketfft and doctest.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Plugins land in `build/AdaptiveRoomEQ_artefacts/Release/{VST3,AU,Standalone}`.
- To build against a local JUCE checkout, add `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.
- For just the analysis core, tests and CLI (no JUCE), add `-DARE_BUILD_PLUGIN=OFF`.

Linux also builds (VST3 + Standalone) with the usual JUCE dependencies. It isn't a release target.

**Before first real use**, set the company name, bundle ID and the two 4-letter plugin codes at the top of `CMakeLists.txt`. Hosts store sessions against those codes, so changing them later orphans saved sessions.

## Testing

```sh
ctest --test-dir build -C Release                                   # C++ unit tests
cd prototype && ROOMEQ_CLI=../build/roomeq_cli pytest               # Python tests + C++ cross-check
./build/AdaptiveRoomEQ_Harness_artefacts/Release/AdaptiveRoomEQ_Harness --out ui.png   # end to end
```

The cross-check (`prototype/tests/test_cpp_port.py`) runs the C++ core and the Python prototype on the same simulated recordings. Every delay, band SNR, grade, reason string and curve must agree to within 1e-6 dB. The fitted correction must agree to within 0.05 dB; locally it's within 2e-6 dB.

The harness drives the real processor against a simulated room. It checks:
- the speakers get exactly the predicted correction and voicing;
- re-measuring through the correction lands near the target;
- undo, hearing the previous correction, custom targets and the state round trip all work;
- dragging handles and target points on the graph works.

It also renders each tab to PNG.

## Routing notes

- **Plugin:** stereo main in/out carries the program to the PA. The mono **Measurement Mic** sidechain carries the mic. If a host only offers stereo sidechains, the first channel is used.
- **Standalone app:** a measurement tool with one input (the mic) and one output (the speaker being swept).
  - Choose the audio device, sample rate and buffer size in **Options → Audio/MIDI Settings**.
  - Pick the channels with the app's own **Mic input** and **Speaker output** menus. They list every channel individually, whereas JUCE's settings dialog only offers stereo pairs.
  - The app only ever outputs the test signal and never sends the mic to the speakers, so it turns off JUCE's default input mute.
  - Measurements play the signal straight out; **Verify** plays it through the correction and voicing EQ.
  - **Measure from music** is plugin-only, because no program passes through the app.
  - The **Test** button in JUCE's settings dialog plays a tone on whichever outputs are active. After picking a **Speaker output**, that's just the chosen one.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them. pocketfft (BSD-3) and doctest (MIT) are also fetched at build time.
