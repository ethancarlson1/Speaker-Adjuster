# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phases 1–3 are implemented, plus Phase 4's show tracking (its reference snapshot and live change warning).
- **Measure:** positions measured with log sweeps or pink noise (or from program material), each capture graded, and every position, the power average and the target shown.
- **Correct:** a conservative minimum-phase correction fitted to the average, applied on your say-so and checked by measuring through it.
- **Voicing:** an 8-band voicing EQ on top of the correction.
- **Loudness:** level calibration, level tracking and ISO 226 loudness compensation, so the balance heard at the reference level holds as the show gets quieter.
- **Show:** a compact show view, and a warning when the room's response moves from what it was at soundcheck.

So far all of this has only been checked against simulated rooms. The next step is a real PA, compared with Smaart or REW.

## Measuring a room (Measure tab)

1. Route the measurement mic to the plugin's **sidechain input**. In the standalone app, pick it in **Mic input** instead.
2. Pick the speaker to measure. In the plugin that's **Speaker** (Left or Right), and the other side stays silent. In the standalone app it's **Speaker output**, any single interface output. The correction is applied to both sides.
3. Pick the **Signal**:
   - **Sweep** (default): a log sine sweep. Each position plays 1–3 sweeps (2, 5 or 10 s) and averages them. It is quick, has the best signal-to-noise ratio, and keeps harmonic distortion out of the result.
   - **Pink noise**: 10, 20 or 30 s of pink noise, analysed against the exact noise that was played (dual-FFT). It needs longer for the same accuracy and doesn't separate out distortion, but a single bang or cough averages out instead of spoiling the capture. 20 s is a good default.

   Both match within about 1 dB above 60 Hz in the simulated rooms.
4. Set **Level** (and the mic gain) so the mic's peaks land in the Mic meter's green zone (see **Mic gain** below). The default is −12 dBFS. It's a peak level for both signals, so pink noise (about 9.5 dB crest factor) plays about 6–7 dB quieter on average than a sweep at the same setting.
5. Press **Measure position** at 3–5 spots across the audience area, at different distances and off-axis. Avoid symmetric spots on the centre line.
6. Each capture is graded **pass / marginal / redo** with the reason (e.g. "low-end noise too high below 180 Hz"). You can rename a capture (double-click), take it out of the average, redo it, or delete it.
7. The graph shows each position (level-aligned), the power average, and the target over the corrected range. With only 1–2 good positions, quick mode smooths more heavily and limits the correction (below).
8. Click a capture to highlight its curve: it's drawn on top, thicker, with the others dimmed and its name in the legend. Verify captures can be highlighted too. Click it again to clear the highlight.

**Mic gain.** The Mic meter in the header has a green target zone, −30 to −10 dBFS, for the mic's peaks. The line under it says what to do:
- ✓ **in the target range**;
- ! **too quiet**, ! **hot** or ! **near clipping**, with about how many dB to turn the mic gain up or down. It aims 5 dB inside the zone, so the level can move a little without leaving it;
- ✗ **clipping** (turn it down at least that much) or ✗ **no mic signal**.

It judges the loudest peaks of the last 3 s, and only while something plays through the plugin (output peaks above −40 dBFS), so a quiet room never reads as "turn it up". Clipping is reported whatever is playing. In the plugin, set the gain with soundcheck music or pink noise at show level. The standalone app only plays test signals, so run one (a pink noise capture is easiest) and set the gain while it plays. The level has to move 1 dB past the edge of the zone before the advice changes, so it doesn't flicker.

**Clear all…** (under the measure buttons) starts the room over. After a confirmation, it deletes:
- every measurement (positions and verify captures), so numbering starts again at 1;
- the applied and previous corrections, so the correction goes flat;
- the loudness level calibration, so the compensation stays flat until you calibrate again;
- the show reference.

The voicing EQ, targets, the mic calibration (it belongs to the mic, not the room) and all settings stay. It can't be undone, and it waits until nothing is measuring, calibrating or storing.

**Nothing plays unless the mic can hear.** Before a sweep, pink noise, a verify, a music capture, the mic calibrator or a re-check starts, the plugin checks that the mic input carried signal (above −100 dBFS) in the last second. If it's dead (not routed, muted, no phantom power), a popup says so and nothing plays.

**The correction is off while measuring.** Every measurement except **Verify** bypasses the correction, the voicing EQ and the level-match make-up, so the fit always sees the PA's own response. The status line says "correction bypassed".
- **Sweeps and pink noise** replace the program with the test signal, played straight to the speaker. The loudness shelves and high-pass go flat at once, under the silence the signal starts with, so it plays exactly as generated.
- **Measure from music** keeps the music playing. The correction, voicing and loudness glide out over 1.5 s, and the 30 s recording starts after that. The audience hears the uncorrected mix for those ~32 s, then everything glides back.
- **Verify** and the loudness **Calibrate level** play through the correction and voicing on purpose: they measure the corrected system, as the audience hears it. The loudness stage is flat for them too.

**Measure from music (30 s)** estimates the response from walk-in music or soundcheck when a sweep isn't possible. It's a dual-FFT against the plugin's output. Both speakers play during it.

**Two clocks.** Pink noise and music compare the output and the mic phase for phase over 20–30 s. If the output and the mic are on different clocks, the delay between them slides during the capture. That happens with two interfaces, or a macOS aggregate device.

Uncorrected, a drift of just 10 ppm (0.001%) makes the high frequencies cancel. The capture fails as "high-frequency noise too high above ~700 Hz", and the top end reads low. Sweeps aren't affected: each is analysed on its own over a few seconds.

So these captures measure the delay in 3 s blocks along the recording and fit a line through them. A drift of 0.2 ppm or more is resampled out before the analysis. The capture then says "output and mic clocks differ by 18.3 ppm (corrected)".

If the delay jumps around instead of sliding steadily (dropouts in an aggregate device), it says so and suggests using one interface. The loudness calibration and **Re-check** get the same correction. One interface for mic and output is still better: it also keeps the loop delay short.

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
   - **Match output level** keeps the overall level where it was (see [Output level match](#output-level-match)).
4. **Verify: measure through the EQ** measures a position with the signal played through the correction and voicing, like the audience hears it. Verify captures (V1, V2…) never change the proposal. Their average is the violet **Verified** curve, and the panel shows how far it is from the target.

**Quick mode:** with one good position the applied correction is half strength and within ±3 dB; with two, 75% and ±6 dB. Full strength needs three.

## Voicing EQ (Voicing tab)

Eight bands (bell, low/high shelf, 12 or 24 dB/octave high/low-pass) after the correction: the engineer's taste layer. Measuring never changes it. Edit a band in the tab, or on the graph:
- drag its numbered handle in the EQ strip (sideways for frequency, up and down for gain);
- use the mouse wheel for Q;
- double-click to switch it on or off.

All voicing and correction settings are automatable parameters. Measurements, the applied and previous corrections, and the custom target are saved with the host session.

## Output level match

**Match output level** (Correct tab, on by default) adds a make-up gain after the correction and voicing, so music comes out of them as loud as it went in. The gain comes from the two EQ curves:
- It's the inverse of their loudness gain on pink noise (equal energy per octave, a stand-in for typical music), weighted like a LUFS meter (ITU-R BS.1770 K-weighting).
- So a big bass cut, which a loudness meter barely notices, gets little make-up. A cut through the presence range gets more.
- It never goes past ±12 dB.

It updates the moment the EQ changes and glides with it, never pumps, and adds no latency. Switching **Correction on**, **Voicing EQ on** or **Hear previous** is therefore a level-matched comparison. The EQ strip shows the gain on its right ("Output level matched: +1.2 dB").

It's exact for pink noise and typical music, and within about a dB for unusually bass-heavy or thin material. It follows the EQ, not the music's level: a quiet song stays quiet.

Loudness compensation isn't levelled. Its boosts are meant to make a quieter show sound fuller. Levelling them would pull the mids down as the boost grows and fight the level tracking.

## Loudness compensation (Loudness tab)

At lower levels we hear less bass and treble (the ISO 226 equal-loudness contours). This stage, after the correction and voicing, raises them as the level drops below the **Reference**, by the difference between the contours at the current level and at the reference. It uses two smooth shelves. Above the reference it does nothing.

It needs the volume turned down **before** the plugin (in the DAW, or on the console feeding it), so the plugin can tell the level from its own output.

**Calibrate once per setup** (plugin only; the standalone app has no program to compensate):
1. Set **Level** on the Measure tab so the noise will be comfortably loud.
2. Press **Calibrate level**. Pink noise plays on both speakers for 12 s, through the correction and voicing.
3. While it plays, read an SPL meter at the mix position (C-weighted, slow) and enter the reading under **Meter read**.

The plugin stores the output level that gave that SPL. With the mic connected, it also stores what the mic heard, for mic tracking and **Re-check**.

**Optional: a calibrated mic.** Put a sound level calibrator (94 or 114 dB) on the mic and press **Calibrate mic**. From then on, a level calibration fills in the SPL the mic heard. That's correct if the mic is at the mix position.

**Settings:**
- **Reference** (95 dB C): the level where the mix sounds right with no compensation.
- **Amount** (100%): how much of the ISO 226 difference to apply.
- **Max boost** (low 8 dB, high 4 dB): the most each shelf ever adds. Low boost never exceeds 12 dB, whatever the setting.
- **Level from**:
  - **Plugin output** (default): steady and deaf to the crowd.
  - **Mic**: the level the mic actually hears, counted only while music plays. It needs a calibration made with the mic connected.
- **Speed** (30 s, from 5 s to 1 min): how long the level is averaged over before the EQ follows it, louder or quieter, so a song's dynamics don't move it. In a simulated song with 20 s verses and choruses 8 dB apart, the level the EQ follows moves about 0.5 dB at 30 s, against about 5 dB with the old 1 s rise and 5 s fall. A loud song after a quiet one is followed within seconds (a 3 s average 6 dB or more above the long one), so a quiet-level bass boost never sits on loud music. Pauses between songs are held.
- **Protective high-pass** (off): a 24 dB/octave high-pass at the PA's measured low-end roll-off. It rises by up to half an octave as the bass boost grows, so the boost doesn't drive the speakers below their range.

**Small changes are ignored.** The EQ follows a change of more than 2 dB straight away. Anything smaller only moves it by a slow drift, over about 30 s. So the EQ doesn't hunt around with the music.

**Re-check level from the music** is for when the gain after the plugin (amp, console fader) changed since calibration. It listens to ~12 s of the show through the mic, with no test signal, and compares the output-to-mic transfer with the calibration, band by band. If the system is louder or quieter by 0.5 dB or more, the calibration follows. If too little of the music reached the mic clearly (crowd, quiet passage), it says so and changes nothing.

The EQ strip shows the compensation at the current level in gold. The tab shows the level, the boosts, and when it was calibrated and re-checked. The calibration is saved with the session.

## Show view and show tracking

**Show view** (the header button, plugin only) is a compact layout for the show. The left panel has show tracking, the bypasses (**Correction**, **Voicing EQ**, **Loudness**, **Match output level**), the loudness readout and **Re-check level**. The capture list goes, and the graph's top panel shows the change since soundcheck; the EQ strip stays. Click it again for the setup view. The choice is saved with the session.

**Show tracking** warns you when the room's response moves away from what it was at soundcheck:
1. At the end of soundcheck, with music or pink noise playing at a normal level, press **Store reference** (show view). It records 30 s through the mic and keeps the output-to-mic response per third octave, 63 Hz–8 kHz.
2. During the show the plugin keeps measuring the same response from the music, 10 s at a time. It compares the last 2 minutes with the reference, band by band, using only the bands the mic heard clearly.
3. A change common to every band is a level change after the plugin (an amp or a fader). It's shown on its own, and flagged if it reaches 3 dB; **Re-check level** brings the loudness calibration up to date with it. What's left is the tonal change.
4. If a band's tonal change reaches **3 dB**, a banner appears on the graph side in both views, e.g. "Since soundcheck: +5.2 dB at 125–250 Hz". It turns red at 6 dB and clears once the change falls below 2 dB. Click the banner for the band-by-band details.
5. The banner's **×** hides it until something new comes up: another band (or the level) is flagged, the change grows from 3 dB to 6 dB, or a band that cleared trips again. The show view's tracking text still shows the change, marked as hidden. The dismissal isn't saved with the session.

Things that aren't the room don't count:
- The measurement is taken after all of the plugin's EQ, so correction, voicing and loudness changes aren't mistaken for the room.
- Crowd noise isn't coherent with the music, so it makes bands count less rather than moving them.
- Blocks during a measurement or calibration (a test signal instead of the show), or with a dead mic, are skipped.

The reference is saved with the session. **Clear** forgets it and stops tracking. Proposing a correction for the change (the rest of Phase 4) comes later.

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
- Start with **Level** around −30 dBFS and the amps turned down. Bring it up until captures grade PASS, with the mic's peaks in the meter's green zone.
- Measure 3 positions and look at the average.
- Then measure the same spots with REW or Smaart (same mic, 1/6-octave smoothing, RMS/power average with SPL alignment) and compare. They should agree within a couple of dB.
- Measure a second time with the plugin to check it's repeatable.

Phase 2's "done when" test:
- Measure 3–5 positions, pick a target, and press **Apply correction**.
- Press **Verify** at two or three of the same spots. The violet Verified curve should sit within a few dB of the target across the corrected range. The Correct tab shows the RMS difference; the simulated room lands under 1 dB.
- Play music and flip **Hear previous** / **Correction on** to listen for artifacts: there should be none, just the tonal change.

Phase 3's "done when" test:
- Calibrate (meter at the mix position), and set **Reference** to the level the system sounds right at.
- Play speech, then music, at the reference. Then turn down 10–15 dB before the plugin.
- A/B **Loudness compensation on** at the lower level. With it on, the balance should sound like it did at the reference: the bass shouldn't thin out, and speech shouldn't lose its presence. The tab shows the boosts it's applying.
- Change the amp gain a few dB during music and press **Re-check level**: it should report the change.

## Layout

| Path | What |
| --- | --- |
| `src/roomeq/` | Analysis core: plain C++17, no JUCE. A port of the Python prototype, using pocketfft |
| `src/plugin/` | JUCE plugin: real-time sweep/noise player and recorder, the correction, voicing and loudness EQ (state-variable filters that glide), output level match, level tracking, calibration, show tracking, background analysis and fitting, UI |
| `prototype/` | Python (NumPy/SciPy) reference implementation, room simulator, tests, review plots |
| `tests/cpp/` | C++ unit tests (doctest) for the core, the real-time recorder, the EQ and the loudness stage |
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
- the speakers get exactly the predicted correction and voicing, plus the output level match's make-up, and pink noise comes out as loud as it goes in;
- re-measuring through the correction lands near the target;
- undo, hearing the previous correction, custom targets and the state round trip all work;
- dragging handles and target points on the graph works;
- a dead mic input refuses every test signal and nothing plays;
- Clear all: refused mid-measurement; afterwards no measurements, corrections, level calibration or show reference, the mic calibration kept, the speakers get just the voicing EQ, and a saved session stays cleared;
- measurements bypass the correction: with correction, voicing, loudness shelves and high-pass all on, a sweep plays exactly as generated from its first sample, and a music capture's speakers get the music uncorrected once it has settled, with everything back afterwards;
- the mic level advice: waiting while nothing plays, too quiet and hot mics brought into the zone by following it, clipping, and no signal;
- selecting a capture (a position or a verify capture) highlights its curve;
- loudness: the mic calibrator, the level calibration in the room, the tracked level against the output, the shelves the speakers get, the deadband, the high-pass, stepping aside during measurements, the re-check finding a 4 dB amp change from music, and at the default 30 s Speed a song with 6 dB dynamics barely moving the EQ while a loud song after a ballad is followed within 6 s;
- show tracking: a reference from music in a room with a crowd, 2.5 minutes of the same room with no warning, a 6 dB low-mid build-up flagged at the right bands, the warning's × keeping it hidden until the top end dulls too, and a block with a sweep in it dropped;
- two clocks: pink noise and music heard through a mic whose clock runs 20 ppm fast still grade PASS, with the drift measured and noted.

It gives the same numbers on every run: audio stops while background analyses run, so thread timing never shifts what follows. It also renders each tab to PNG.

## Routing notes

- **Plugin:** stereo main in/out carries the program to the PA. The mono **Measurement Mic** sidechain carries the mic. If a host only offers stereo sidechains, the first channel is used.
- **One interface if you can:** with the mic on one device and the PA feed on another (or a macOS aggregate device), their clocks drift. Pink noise and music correct for that (see [Two clocks](#measuring-a-room-measure-tab)), but the loop delay gets long, and an aggregate device that drops samples can't be corrected.
- **Standalone app:** a measurement tool with one input (the mic) and one output (the speaker being swept).
  - Choose the audio device, sample rate and buffer size in **Options → Audio/MIDI Settings**.
  - Pick the channels with the app's own **Mic input** and **Speaker output** menus. They list every channel individually, whereas JUCE's settings dialog only offers stereo pairs.
  - The app only ever outputs the test signal and never sends the mic to the speakers, so it turns off JUCE's default input mute.
  - Measurements play the signal straight out; **Verify** plays it through the correction and voicing EQ.
  - **Measure from music** and loudness compensation are plugin-only, because no program passes through the app.
  - The **Test** button in JUCE's settings dialog plays a tone on whichever outputs are active. After picking a **Speaker output**, that's just the chosen one.

**Latency:** none. Every EQ stage is a minimum-phase IIR filter processed in place, with no lookahead, so the plugin reports 0 samples. The round trip is set by the interface and host buffer size.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them. pocketfft (BSD-3) and doctest (MIT) are also fetched at build time.
