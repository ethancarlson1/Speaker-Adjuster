# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phase 1 (measurement tool) is in progress. The plugin project builds and passes audio through unchanged. The DSP is being prototyped in Python in [`prototype/`](prototype/) before being ported to C++.

## Layout

| Path | What |
| --- | --- |
| `CMakeLists.txt` | JUCE plugin project: VST3 + Standalone everywhere, AU on macOS |
| `src/plugin/` | Plugin processor and editor |
| `prototype/` | Python (NumPy/SciPy) prototypes of the analysis DSP, room simulator and tests |
| `.github/workflows/ci.yml` | macOS + Windows plugin builds (validated with pluginval) and prototype tests |

## Building the plugin

Requirements: CMake 3.22+, a C++17 compiler (Xcode on macOS, Visual Studio 2019+ on Windows). The first configure downloads JUCE 8.0.15 automatically.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Builds land in `build/AdaptiveRoomEQ_artefacts/Release/{VST3,AU,Standalone}`. To use a local JUCE checkout instead of downloading one, add `-DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE`.

Linux also builds (VST3 + Standalone) with the usual JUCE dependencies. It isn't a release target.

**Before first real use**, set the company name, bundle ID and the two 4-letter plugin codes at the top of `CMakeLists.txt`. Hosts store sessions against those codes, so changing them later orphans saved sessions.

## Routing

- **Main bus:** stereo in, stereo out. The program goes through this and on to the PA.
- **Sidechain input ("Measurement Mic"):** mono. Route the measurement mic here. If a host only offers stereo sidechains, the first channel is used.

In the standalone app, device inputs 1–2 feed the main bus and input 3 is the mic.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them.
