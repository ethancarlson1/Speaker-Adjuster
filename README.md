# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phase 1 (measurement tool) is implemented. The plugin measures positions with log sweeps (or from program material), grades each capture, and shows every position, the power average and the target. Audio passes through unchanged except while a sweep plays. The correction EQ comes in Phase 2. Phase 1 has only been checked against simulated rooms; the next step is a real PA and a comparison with Smaart or REW.

## Measuring a room

1. Route the measurement mic to the plugin's **sidechain input**. In the standalone app, pick it in **Mic input** instead.
2. Pick the speaker to sweep. In the plugin that's **Sweep speaker** (Left or Right), and the other side stays silent. In the standalone app it's **Speaker output**, any single interface output. The Phase 2 correction will be applied to both sides.
3. Set **Sweep level** so the mic meter peaks well below 0 dBFS. The default is −12 dBFS.
4. Press **Measure position** at 3–5 spots across the audience area, at different distances and off-axis. Avoid symmetric spots on the centre line. Each position plays 1–3 sweeps (2, 5 or 10 s) and averages them.
5. Each capture is graded **pass / marginal / redo** with the reason (e.g. "low-end noise too high below 180 Hz"). You can rename a capture (double-click), take it out of the average, redo it, or delete it.
6. The graph shows each position (level-aligned), the power average, and the flat target over the PA's usable range. With only 1–2 good positions, quick mode smooths more heavily and limits how strong the later correction will be.

**Measure from music (30 s)** estimates the response from walk-in music or soundcheck when a sweep isn't possible (dual-FFT against the plugin input). Both speakers play during it.

Measurements are saved with the host session.

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
- **Plugin:** the output the sweep plays on, into the input you route to the sidechain.
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

This is Phase 1's "done when" test.
- Start with **Sweep level** around −30 dBFS and the amps turned down. Bring it up until captures grade PASS, with the mic meter well below 0 dBFS.
- Measure 3 positions and look at the average.
- Then measure the same spots with REW or Smaart (same mic, 1/6-octave smoothing, RMS/power average with SPL alignment) and compare. They should agree within a couple of dB.
- Measure a second time with the plugin to check it's repeatable.

## Layout

| Path | What |
| --- | --- |
| `src/roomeq/` | Analysis core: plain C++17, no JUCE. A port of the Python prototype, using pocketfft |
| `src/plugin/` | JUCE plugin: real-time sweep player/recorder, background analysis engine, UI |
| `prototype/` | Python (NumPy/SciPy) reference implementation, room simulator, tests, review plots |
| `tests/cpp/` | C++ unit tests (doctest) for the core and the real-time recorder |
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

The cross-check (`prototype/tests/test_cpp_port.py`) runs the C++ core and the Python prototype on the same simulated recordings. Every delay, band SNR, grade, reason string and curve must agree to within 1e-6 dB.

## Routing notes

- **Plugin:** stereo main in/out carries the program to the PA. The mono **Measurement Mic** sidechain carries the mic. If a host only offers stereo sidechains, the first channel is used.
- **Standalone app:** a measurement tool with one input (the mic) and one output (the speaker being swept).
  - Choose the audio device, sample rate and buffer size in **Options → Audio/MIDI Settings**.
  - Pick the channels with the app's own **Mic input** and **Speaker output** menus. They list every channel individually, whereas JUCE's settings dialog only offers stereo pairs.
  - The app only ever outputs the sweep and never sends the mic to the speakers, so it turns off JUCE's default input mute.
  - **Measure from music** is plugin-only, because no program passes through the app.
  - The **Test** button in JUCE's settings dialog plays a tone on whichever outputs are active. After picking a **Speaker output**, that's just the chosen one.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them. pocketfft (BSD-3) and doctest (MIT) are also fetched at build time.
