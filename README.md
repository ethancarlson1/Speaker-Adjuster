# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phase 1 (measurement tool) is implemented. The plugin measures positions with log sweeps (or from program material), grades each capture, and shows every position, the power average and the target. Audio passes through unchanged except while a sweep plays. The correction EQ comes in Phase 2. Phase 1 has only been checked against simulated rooms; the next step is a real PA and a comparison with Smaart or REW.

## Measuring a room

1. Route the measurement mic to the plugin's **sidechain input** (in the standalone app, device input 3).
2. Pick **Sweep speaker** (Left or Right). The sweep plays on that speaker only, and the other side stays silent. The Phase 2 correction will be applied to both sides.
3. Set **Sweep level** so the mic meter peaks well below 0 dBFS. The default is −12 dBFS.
4. Press **Measure position** at 3–5 spots across the audience area, at different distances and off-axis. Avoid symmetric spots on the centre line. Each position plays 1–3 sweeps (2, 5 or 10 s) and averages them.
5. Each capture is graded **pass / marginal / redo** with the reason (e.g. "low-end noise too high below 180 Hz"). You can rename a capture (double-click), take it out of the average, redo it, or delete it.
6. The graph shows each position (level-aligned), the power average, and the flat target over the PA's usable range. With only 1–2 good positions, quick mode smooths more heavily and limits how strong the later correction will be.

**Measure from music (30 s)** estimates the response from walk-in music or soundcheck when a sweep isn't possible (dual-FFT against the plugin input). Both speakers play during it.

Measurements are saved with the host session.

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
- **Standalone app:** JUCE's standalone can't use sidechains, so the app takes three inputs: 1–2 program, 3 mic. JUCE mutes standalone inputs by default to avoid feedback. The app turns that off when its window opens, because the mic has to be heard.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them. pocketfft (BSD-3) and doctest (MIT) are also fetched at build time.
