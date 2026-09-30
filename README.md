# Adaptive Room EQ

A VST3/AU plugin (plus a standalone app) that measures a PA in the room, corrects it, and keeps the tonal balance consistent as the volume drops and the room fills. See [SPEC.md](SPEC.md) for the full design.

**Status:** Phases 1–3 are implemented.
- **Measure:** positions measured with log sweeps or pink noise (or from program material), each capture graded, and every position, the power average and the target shown.
- **Correct:** a conservative minimum-phase correction fitted to the average, applied on your say-so and checked by measuring through it.
- **Voicing:** an 8-band voicing EQ on top of the correction.
- **Level compensation:** level calibration, level tracking and equal-loudness compensation (ISO 226:2003), so the balance approved at the reference level holds as the show gets quieter.
- **Show:** a compact show view with the mic's spectrogram, the EQ over it, and an SPL meter.
- **Zones:** one instance per zone (mains, subs, front fill, delay speakers) on a mono or stereo track, each judged on its own band, with a delay and polarity to line it up.

So far all of this has only been checked against simulated rooms. The next step is a real PA, compared with Smaart or REW.

## Measuring a room (Measure tab)

1. Route the measurement mic to the plugin's **sidechain input**. In the standalone app, pick it in **Mic input** instead.
2. Pick the speaker to measure. In the plugin that's **Speaker** (Left or Right), and the other side stays silent. On a mono track there's only one, so the choice goes. In the standalone app it's **Speaker output**, any single interface output. The correction is applied to both sides.
3. Pick the **Signal**:
   - **Sweep** (default): a log sine sweep. Each position plays 1–3 sweeps (2, 5 or 10 s) and averages them. It is quick, has the best signal-to-noise ratio, and keeps harmonic distortion out of the result.
   - **Pink noise**: 10, 20 or 30 s of pink noise, analysed against the exact noise that was played (dual-FFT). It needs longer for the same accuracy and doesn't separate out distortion, but a single bang or cough averages out instead of spoiling the capture. 20 s is a good default.

   Both match within about 1 dB above 60 Hz in the simulated rooms.
4. Set **Level** (and the mic gain) so the mic's peaks land in the Mic meter's green zone (see **Mic gain** below). The default is −12 dBFS. It's a peak level for both signals, so pink noise (about 9.5 dB crest factor) plays about 6–7 dB quieter on average than a sweep at the same setting.
5. Press **Measure position** at 3–5 spots across the audience area, at different distances and off-axis. Avoid symmetric spots on the centre line.
6. Each capture is graded **pass / marginal / redo** with the reason (e.g. "low-end noise too high below 180 Hz"). You can rename a capture (double-click), take it out of the average, redo it, or delete it.

   Under its badge is the direct sound's arrival, e.g. "sweep 25.40 ms", with a dot for how far to trust it: green high, amber medium, red low (see [Alignment](#alignment)).
7. The graph shows each position (level-aligned), the power average, and the target over the corrected range. With only 1–2 good positions, quick mode smooths more heavily and limits the correction (below).
8. Click a capture to highlight its curve: it's drawn on top, thicker, with the others dimmed and its name in the legend. Verify captures can be highlighted too. Click it again to clear the highlight.
9. The selected capture's **Measurement quality** shows under the measure buttons (see below).

**Measurement quality.** The grading's numbers in words, for the selected capture. Each row is rated **Excellent / Good / Fair / Poor** from its worst graded octave band, as the grade is. Good and Fair start at the grading's pass and marginal limits, so a PASS capture is Good or better.

| Row | From | Excellent | Good | Fair |
|---|---|---|---|---|
| Signal-to-noise | band SNR | ≥ 30 dB | ≥ 20 dB | ≥ 10 dB |
| Coherence (music and pink noise) | the band's mean coherence over the bins the program excited | ≥ 0.95 | ≥ 0.85 | ≥ 0.6 |
| Repeatability (two or more sweeps) | the worst 1/3-octave spread between repeats, beyond what the noise explains | ≤ 0.5 dB | ≤ 1 dB | ≤ 3 dB |

- **Missing rows:** sweeps have no coherence, and a single sweep or a music or noise capture has no repeats to compare. Those rows show a dash.
- **Usable range:** where this capture is within 10 dB of its reference band (1-octave smoothed), found as the session's is.
- **Overall confidence:** **high**; **medium** when anything is Fair or the grade is marginal; **low** when anything is Poor, the grade is redo, or no band was heard. The reason is shown under it.

**Details…** opens the raw numbers:
- each octave band's level, SNR, coherence, repeat spread (raw and beyond the noise) and grade;
- the arrival and its confidence reasons;
- any clock drift that was corrected;
- for sweeps, the impulse response from 5 ms before its peak to 95 ms after, as an energy-time curve in dB, with the arrival marked. It's kept with the session.

**Mic gain.** The Mic meter in the header has a green zone, −30 to −10 dBFS: set the mic gain so its peaks sit there. In the plugin, do it with soundcheck music or pink noise at show level. The standalone app only plays test signals, so run one (a pink noise capture is easiest) and set the gain while it plays. The bar turns red above −3 dBFS.

**Clear all…** (under the measure buttons) starts the room over. After a confirmation, it deletes:
- every measurement (positions and verify captures), so numbering starts again at 1;
- the applied and previous corrections, so the correction goes flat;
- the level calibration, so the compensation stays flat until you calibrate again.

The voicing EQ, targets, the mic calibration (it belongs to the mic, not the room) and all settings stay. It can't be undone, and it waits until nothing is measuring or calibrating.

**Nothing plays unless the mic can hear.** Before a sweep, pink noise, a verify, a music capture, the mic calibrator or a re-check starts, the plugin checks that the mic input carried signal (above −100 dBFS) in the last second. If it's dead (not routed, muted, no phantom power), a popup says so and nothing plays.

**The correction is off while measuring.** Every measurement except **Verify** bypasses the correction, the voicing EQ and the level-match make-up, so the fit always sees the PA's own response. The status line says "correction bypassed".
- **Sweeps and pink noise** replace the program with the test signal, played straight to the speaker. The level compensation's shelves and high-pass go flat at once, under the silence the signal starts with, so it plays exactly as generated.
- **Measure from music** keeps the music playing. The correction, voicing and level compensation glide out over 1.5 s, and the 30 s recording starts after that. The audience hears the uncorrected mix for those ~32 s, then everything glides back.
- **Verify** and the level compensation's **Calibrate level** play through the correction and voicing on purpose: they measure the corrected system, as the audience hears it. The level compensation is flat for them too.

**Measure from music (30 s)** estimates the response from walk-in music or soundcheck when a sweep isn't possible. It's a dual-FFT against the plugin's output. Both speakers play during it.

**Two clocks.** Pink noise and music compare the output and the mic phase for phase over 20–30 s. If the output and the mic are on different clocks, the delay between them slides during the capture. That happens with two interfaces, or a macOS aggregate device.

Uncorrected, a drift of just 10 ppm (0.001%) makes the high frequencies cancel. The capture fails as "high-frequency noise too high above ~700 Hz", and the top end reads low. Sweeps aren't affected: each is analysed on its own over a few seconds.

So these captures measure the delay in 3 s blocks along the recording and fit a line through them. A drift of 0.2 ppm or more is resampled out before the analysis. The capture then says "output and mic clocks differ by 18.3 ppm (corrected)".

If the delay jumps around instead of sliding steadily (dropouts in an aggregate device), it says so and suggests using one interface. The level calibration and **Re-check** get the same correction. One interface for mic and output is still better: it also keeps the loop delay short.

## Zones (Zone tab)

Use one instance per zone, each on its own track or output bus:
- **Mains**, usually stereo;
- **Subs**, **Front fill** and **Delay** speakers, often mono tracks.

The plugin runs mono or stereo, whichever the host's track is. On a mono track the test signal plays on its one channel.

**Zone** sets what measurements are judged against, and a starting correction range:

| Zone | Judged on | Correction range to start |
| --- | --- | --- |
| Mains | 250 Hz–4 kHz | 20 Hz–20 kHz |
| Subs | 40–100 Hz | 20–150 Hz |
| Front fill | 250 Hz–4 kHz | 80 Hz–20 kHz |
| Delay | 250 Hz–4 kHz | 80 Hz–20 kHz |

- **The reference band** is where a speaker is expected to play. A sub has nothing at 1 kHz, so it's judged on 40–100 Hz. The band is used for:
  - grading: the band levels, and which ends are outside the speaker's range;
  - lining the positions up with each other;
  - finding the usable range;
  - placing the target.
- **Changing the zone after measuring** re-grades the captures you have, exactly as measuring them again would. SNR and repeat spread don't depend on the band, so nothing needs re-measuring.
- **The correction range** is only a starting point: choosing a zone sets **Correct from / up to** on the Correct tab, and you can change them after. Opening a saved session keeps its own range.

**Delay** (0–300 ms, in 0.01 ms steps) lines a fill or delay speaker up with the mains.
- It shows the distance too, e.g. "12.50 ms (4.29 m / 14.1 ft)", at 343 m/s.
- Double-click to type a time, or a distance ("25 m", "82 ft"). Hold Ctrl (Cmd on a Mac) while dragging for fine steps.
- Start at the extra distance the mains' sound travels to reach the speaker's area, then fine-tune by ear or with **Verify**.
- Changes crossfade over 20 ms, so moving it never clicks.
- Fractions of a sample are band-limited, flat to within 0.002 dB up to 20 kHz at 48 kHz.
- It isn't reported to the host as latency: it's there on purpose.

**Invert polarity** flips the output, for a sub or fill that cancels the mains around the crossover. It crossfades too.

Both follow the same rule as the correction:
- **Measurements bypass them,** so the fit sees the speaker's own response and the loop delay stays short.
- **Verify includes them,** so it measures what the audience hears. The level compensation's **Calibrate level** does too.

The standalone app has the **Zone** choice (for grading) but no delay or polarity: it only plays test signals.

### Alignment

The **Alignment** section of the Zone tab turns arrival times into a delay for this zone. It suggests; nothing changes until you press **Apply**.

**Arrivals.** Every measurement records when the direct sound arrived:
- **Sweeps:** the first peak of the impulse response within 6 dB of the strongest, refined below a sample (to about 0.004 ms). A floor bounce or a wall that's nearly as loud, arriving later, doesn't move it.
- **Pink noise and music:** the dual-FFT's delay, to the nearest sample.
- It's a loop delay: the interface and host round trip plus the flight time. Arrivals are compared with each other, so the round trip cancels when both zones use the same interface.

**Confidence** (the dot in the capture list, and the word in the lists here):
- **High:** a clean, repeatable arrival, well above the noise.
- **Medium:** the repeat sweeps differ by more than 0.05 ms; a later arrival is more than 3 dB stronger than the direct sound; an earlier arrival sits within 12 dB of the strongest (the direct sound may be partly blocked); 10–20 dB SNR where the arrival is set; or it came from pink noise or music.
- **Low:** the repeats disagree by more than 0.25 ms (something moved), or under 10 dB SNR where the arrival is set.

**System latency** (optional): **Loopback** measures the round trip with a cable from the output straight into the mic input. You can also type it. It's taken off arrivals to give flight times. A measurement that isn't a clean, flat loopback (a speaker in a room) is refused and changes nothing.

**Lining a fill or delay speaker up with the mains:**
1. Put the mic where the fill (or delay) and the mains overlap.
2. Measure the mains there, in the mains' instance.
3. Measure this zone there, in this instance.
4. **Line up with** lists the other instances of the plugin in this DAW, with their zone and track name ("Mains (PA)"), mains first. Pick it, its measurement (**Their arrival**) and this zone's (**This zone's**).
5. It suggests this zone's delay: the mains' arrival (plus the mains' own zone delay, if any) minus this zone's. For example, the mains at 51.65 ms and a fill at 25.40 ms gives 26.25 ms (9.00 m / 29.5 ft).
6. **Apply** sets this zone's **Delay** to it. Then **Verify** measures through it: the fill should arrive with the mains.

Some engineers add a few milliseconds more so the mains arrive first and the sound stays anchored on the stage; add it to **Delay** by hand. If this zone already arrives after the mains at that spot, it says so and suggests nothing.

**Instances the plugin can't see.** Instances share what they measured when the host runs them in the same process, which most do. Hosts that run each plugin separately keep them apart. Then choose **Type the main arrival** and type the mains' arrival as their instance showed it.

**Lining a sub up with the mains.** A sub's impulse response has a broad peak, so its arrival says little. A Subs zone lines up by phase instead:
1. Put the mic where the mains and the sub overlap. That's usually a typical listening spot, not right against either box.
2. Sweep the mains there, in the mains' instance (with the sub muted).
3. Sweep the sub there, in the sub's instance (with the mains muted).
4. Pick the mains' instance, **Their sweep** and **This sub's**. Pink noise and music don't keep the phase this needs, so only sweeps are listed.
5. It finds the crossover region: where the two are within 10 dB of each other around the frequency where they cross, in 30–300 Hz (1/6-octave smoothed).
6. It predicts the sum there for every sub delay from −20 to +20 ms (0.01 ms steps), in both polarities. It suggests the loudest one, e.g. "Crossover 71–135 Hz (crossing at 82 Hz). Suggested: delay 10.89 ms, polarity normal. Over the crossover: 1.6 dB louder than now, 1.3 dB short of a perfect sum."
7. **Apply** sets the sub's **Delay** and **Invert polarity**.

How it chooses and what it tells you:
- **Near-ties:** when choices sum within 0.25 dB of each other (half a period apart with the polarity flipped, or a whole period apart), it prefers a delay the sub can apply itself, then normal polarity, then the smallest delay.
- **Sub arriving late:** if the best answer is a negative delay, it says to delay the mains by that much instead (and anything lined up with them).
- **Confidence:** medium under 20 dB SNR over the crossover, low under 10 dB. Also medium when the two overlap over less than a third of an octave, or when a delay a whole period away sums within 0.3 dB (the overlap doesn't pin the timing down).
- **What it uses:** each sweep keeps its complex response from 20 Hz to 1 kHz. It's taken from the start of the loop to 500 ms after the peak, so it holds the direct sound even when the room modes build up to a louder peak later. The response is saved with the session.
- **What it needs:** the mains' instance in this DAW (a response can't be typed in). When both instances have a measured system latency it's taken off each; otherwise the two are assumed to share an interface.

## Correcting (Correct tab)

1. Pick a **Target**:
   - **Flat**.
   - **House:** +4 dB below ~80 Hz easing to 0 by 250 Hz, and −1 dB/octave above 2 kHz (−3 dB at 16 kHz).
   - **Speech:** −6 dB at 75 Hz, +2 dB at 2–4 kHz, and −3 dB at 16 kHz.
   - **Custom:** drag its points on the graph; double-click to add or remove one. The **Targets…** menu saves it as a file, loads saved ones, or starts one from a preset.

   The target is placed on the average by its level over the zone's reference band (250 Hz–4 kHz, or 40–100 Hz for subs), so the correction reshapes the ends rather than moving the overall level.
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

Level compensation isn't levelled. Its boosts are meant to make a quieter show sound fuller. Levelling them would pull the mids down as the boost grows and fight the level tracking.

## Level compensation (Level comp tab)

At lower levels we hear less bass and treble than at higher ones: equal-loudness behaviour. This stage, after the correction and voicing, keeps the tonal balance you approved at the **Reference level** similar when the show plays quieter:
- It measures the **current level** (dB C at the mix position, from the calibration) and the change from the reference, e.g. 95 → 87 dB(C) is −8 dB.
- For that change it applies the change in shape of the ISO 226:2003 equal-loudness contours, as two smooth shelves (low and high).
- At or above the reference it does nothing.

**What it models, and what it assumes:**
- **Contours:** ISO 226:2003 (the second edition; the 2023 edition isn't encoded). They're the standard's formula with its Table 1 parameters at its 29 frequencies, 20 Hz–12.5 kHz. Between those they're interpolated in log frequency, and beyond them held. The tests pin the 40 and 80 phon contours to 0.01 dB.
- **The reference picks the region, the level change sets the amount.** ISO 226 is about pure tones in phon, and a broadband dB(C) reading of music isn't a phon value. So the reference level in dB(C) is used only to choose where on the contour family to read the change. That's an approximation, but a mild one: from any reference between 80 and 100 dB(C), an 8 dB drop asks for the same boost within about 0.1 dB.
- **Range:** the formula is specified for 20–90 phon (20–80 phon from 5 kHz up). Outside that it's used as is, and anything below 20 phon is treated as 20.
- **Two shelves, not a full contour:** the shelves' frequency and Q are fitted once per reference level, and only their gains follow the level. No FIR, no lookahead: the stage adds no latency and very little CPU.

It needs the volume turned down **before** the plugin (in the DAW, or on the console feeding it), so the plugin can tell the level from its own output.

**Calibrate once per setup** (plugin only; the standalone app has no program to compensate):
1. Set **Level** on the Measure tab so the noise will be comfortably loud.
2. Press **Calibrate level**. Pink noise plays on both speakers for 12 s, through the correction and voicing.
3. While it plays, read an SPL meter at the mix position (C-weighted, slow) and enter the reading under **Meter read**.

The plugin stores the output level that gave that SPL. With the mic connected, it also stores what the mic heard, for mic tracking and **Re-check**.

**Optional: a calibrated mic.** Put a sound level calibrator (94 or 114 dB) on the mic and press **Calibrate mic**. From then on, a level calibration fills in the SPL the mic heard. That's correct if the mic is at the mix position.

**Settings:**
- **Reference level** (95 dB C): the level where you approved the system's sound.
- **Amount** (75%, **Natural**): how much of the predicted change to apply. The presets are **Subtle** 50%, **Natural** 75% and **Full** 100%; any other value reads Custom. The full theoretical change doesn't always sound best on mastered music, so the default is 75% until listening tests say otherwise.
- **Max boost** (low 8 dB, high 4 dB): the most each shelf ever adds. Low boost never exceeds 12 dB, whatever the setting.
- **The PA's low end limits the bass boost.** With measurements, the usable range's low end (where the measured response is 10 dB down) is compared with the low shelf's frequency (about 134 Hz at a 95 dB reference). An octave or more below it, the full boost is allowed. Closer than that, the allowance shrinks in proportion (in octaves), to none when the PA's range starts at the shelf's frequency. For example, tops whose usable range starts at 95 Hz get about half their bass boost; the sub instance, playing down to 30 Hz, gets all of its own. The tab says when this is limiting. Without measurements the settings apply as they are.
- **Level from**:
  - **Plugin output** (default): steady and deaf to the crowd.
  - **Mic**: the level the mic actually hears, counted only while music plays. It needs a calibration made with the mic connected.
- **Speed** (30 s, from 5 s to 1 min): how long the level is averaged over before the EQ follows it, louder or quieter, so a song's dynamics don't move it. In a simulated song with 20 s verses and choruses 8 dB apart, the level the EQ follows moves about 0.5 dB at 30 s, against about 5 dB with the old 1 s rise and 5 s fall. A loud song after a quiet one is followed within seconds (a 3 s average 6 dB or more above the long one), so a quiet-level bass boost never sits on loud music. Pauses between songs are held.
- **Protective high-pass** (off): a 24 dB/octave high-pass at the PA's measured low-end roll-off. It rises by up to half an octave as the bass boost grows, so the boost doesn't drive the speakers below their range.

**Small changes are ignored.** The EQ follows a change of more than 2 dB straight away. Anything smaller only moves it by a slow drift, over about 30 s. So the EQ doesn't hunt around with the music.

**Re-check level from the music** is for when the gain after the plugin (amp, console fader) changed since calibration. It listens to ~12 s of the show through the mic, with no test signal, and compares the output-to-mic transfer with the calibration, band by band. If the system is louder or quieter by 0.5 dB or more, the calibration follows. If too little of the music reached the mic clearly (crowd, quiet passage), it says so and changes nothing.

The EQ strip shows the compensation at the current level in gold. The tab shows the current level and its change from the reference, the boosts, any limit from the PA's low end, and when it was calibrated and re-checked. The calibration is saved with the session.

## Show view

**Show view** (the header button, plugin only) is a compact layout for the show. Click it again for the setup view. The choice is saved with the session.

The left panel has:
- the bypasses (**Correction**, **Voicing EQ**, **Level comp.**, **Match output level**);
- the level compensation readout (current level, change from the reference, boosts) and **Re-check level**;
- the SPL meter.

The capture list goes, and the graph side shows **the mic's spectrogram with the EQ over it**, 20 Hz–20 kHz. A **Both / Spectrogram / EQ** switch at the right of its legend row picks what's shown, and the choice is saved with the session.
- **Both** (the default) draws the correction, voicing and level compensation curves and the voicing handles over the spectrogram. Drag a handle straight to where you see a build-up. Hovering reads out the frequency and the EQ there.
- **Spectrogram** shows it alone, with its time labels and colour key; the handles step aside.
- **EQ** shows the curves alone, as in the setup view.

The spectrogram covers the last 20 s, newest at the top. Each column is the energy in its slice of the axis, like an RTA, so pink noise reads flat. Colour is level against the loudest of the last few seconds, over 60 dB. It's an 8192-point FFT (about 6 Hz resolution) about 23 times a second, run by the UI only while the show view is on screen.

**SPL at the mic** reads dB SPL from the measurement mic, so it needs the mic calibration (calibrator, Level comp tab). Until then it says so. It shows:
- the live **dB(A) fast** (125 ms) level, and its max;
- **LAeq** and **LCeq** over the last 15 minutes (or as much as it has heard so far);
- **Reset**, which starts the Leqs and the max over.

The weighting is IEC 61672 A and C on the audio thread, and it keeps running with the editor closed. Seconds with nothing from the mic aren't counted, since a dead mic isn't silence.

**No "change since soundcheck".** An earlier version stored the room's output-to-mic response at soundcheck and warned when it moved during the show. It was removed: the mic also hears the stage (drums, amps, wedges) and other zones playing the same material as the PA, so the comparison moved with each song's instrumentation more than with the room.

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
- A/B **Level compensation on** at the lower level. With it on, the balance should sound like it did at the reference: the bass shouldn't thin out, and speech shouldn't lose its presence. The tab shows the boosts it's applying.
- Change the amp gain a few dB during music and press **Re-check level**: it should report the change.

## Layout

| Path | What |
| --- | --- |
| `src/roomeq/` | Analysis core: plain C++17, no JUCE. A port of the Python prototype, using pocketfft |
| `src/plugin/` | JUCE plugin: real-time sweep/noise player and recorder, the correction, voicing and level compensation EQ (state-variable filters that glide), output level match, level tracking, calibration, the zone's delay, background analysis and fitting, UI |
| `prototype/` | Python (NumPy/SciPy) reference implementation, room simulator, tests, review plots |
| `tests/cpp/` | C++ unit tests (doctest) for the core, the real-time recorder, the EQ, the level compensation stage and the zone's delay |
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

The cross-check (`prototype/tests/test_cpp_port.py`) runs the C++ core and the Python prototype on the same simulated recordings. Every delay, band SNR, coherence, grade, quality rating, reason string and curve must agree to within 1e-6 dB. The fitted correction must agree to within 0.05 dB; locally it's within 2e-6 dB.

The harness drives the real processor against a simulated room. It checks:
- the speakers get exactly the predicted correction and voicing, plus the output level match's make-up, and pink noise comes out as loud as it goes in;
- re-measuring through the correction lands near the target;
- undo, hearing the previous correction, custom targets and the state round trip all work;
- dragging handles and target points on the graph works;
- a dead mic input refuses every test signal and nothing plays;
- Clear all: refused mid-measurement; afterwards no measurements, corrections or level calibration, the mic calibration kept, the speakers get just the voicing EQ, and a saved session stays cleared;
- measurements bypass the correction: with correction, voicing, level compensation shelves and high-pass all on, a sweep plays exactly as generated from its first sample, and a music capture's speakers get the music uncorrected once it has settled, with everything back afterwards;
- selecting a capture (a position or a verify capture) highlights its curve and shows its measurement quality: a clean sweep high confidence, the rumble capture low (poor SNR at 31.5 Hz), the music capture with its coherence; Details with 100 ms of impulse response; still shown in the smallest window; and quality, coherence and impulse responses surviving the saved session;
- level compensation: Amount at 75% (Natural) by default and the presets setting and following it, the PA's measured low end reaching it, the mic calibrator, the level calibration in the room, the tracked level against the output, the shelves the speakers get, the deadband, the high-pass, stepping aside during measurements, the re-check finding a 4 dB amp change from music, and at the default 30 s Speed a song with 6 dB dynamics barely moving the EQ while a loud song after a ballad is followed within 6 s;
- the show view's SPL meter reading the 94 dB calibrator as 94.0 dB(A), LAeq and LCeq; the spectrogram placing a 1 kHz tone at 1 kHz and keeping up with the music; hovering it reading out the EQ; a voicing handle dragging over the spectrogram but not in Spectrogram-only; and the panel choice saved with the session;
- zones: a mono track (and 5.1 refused), a sub measured on it and graded on 40–100 Hz, its fit staying at 150 Hz and below and cutting the room mode, switching the zone re-grading the captures and back again giving exactly the grades measured, and a saved session keeping its own range;
- alignment: a loopback setting the system latency (a speaker in a room refused as one); the mains at 12 m and a front fill at 3 m measured at the same spot; the fill instance seeing "Mains (PA)" through the registry, suggesting 26.25 ms, changing nothing until Apply, and then Verify arriving with the mains; a typed arrival; and a closed instance leaving the registry;
- sub alignment: the mains (high-passed at 90 Hz, 1.5 ms of processing, 9 m) and a sub (low-passed at 90 Hz, 6 m) swept at the same spot; the sub's panel suggesting about 10.9 ms, normal polarity; then the mains' sweep fed through the sub instance too, so the two are measured together as they were and with the suggestion applied: the measured gain over the crossover matches the prediction (+1.62 dB against +1.64 dB), and the sub's response is saved with the session;
- delay and polarity: 12.50 ms inverted comes out 600 samples later and flipped, 12.51 ms is flat to 10 kHz, a typed distance converts, a measurement bypasses a 100 ms delay while Verify measures through it, and both are saved;
- two clocks: pink noise and music heard through a mic whose clock runs 20 ppm fast still grade PASS, with the drift measured and noted.

It gives the same numbers on every run: audio stops while background analyses run, so thread timing never shifts what follows. It also renders each tab to PNG.

## Routing notes

- **Plugin:** the main in/out carries the program to the PA, mono or stereo as the host's track is. The mono **Measurement Mic** sidechain carries the mic. If a host only offers stereo sidechains, the first channel is used.
- **One interface if you can:** with the mic on one device and the PA feed on another (or a macOS aggregate device), their clocks drift. Pink noise and music correct for that (see [Two clocks](#measuring-a-room-measure-tab)), but the loop delay gets long, and an aggregate device that drops samples can't be corrected.
- **Standalone app:** a measurement tool with one input (the mic) and one output (the speaker being swept).
  - Choose the audio device, sample rate and buffer size in **Options → Audio/MIDI Settings**.
  - Pick the channels with the app's own **Mic input** and **Speaker output** menus. They list every channel individually, whereas JUCE's settings dialog only offers stereo pairs.
  - The app only ever outputs the test signal and never sends the mic to the speakers, so it turns off JUCE's default input mute.
  - Measurements play the signal straight out; **Verify** plays it through the correction and voicing EQ.
  - **Measure from music** and level compensation are plugin-only, because no program passes through the app.
  - The **Test** button in JUCE's settings dialog plays a tone on whichever outputs are active. After picking a **Speaker output**, that's just the chosen one.

**Zero added plugin latency.** The plugin adds no buffering latency and reports 0 samples to the host:
- Every EQ stage (correction, voicing, level compensation, output level match) is a minimum-phase IIR filter processed in place, sample by sample.
- There's no lookahead, FIR or convolution, frequency-domain block processing, or resampling in the audio path.
- Measurement analysis uses FFTs, but it runs in the background, off the audio path.

The round trip is set by the interface and host buffer size. The zone's **Delay** is separate: it's there on purpose to line speakers up, so it isn't plugin processing latency and isn't reported. The harness checks the reported latency is 0 with everything on, with a 300 ms zone delay, at another sample rate and block size, on a mono track and in the standalone app.

## Licensing note

JUCE 8 is dual-licensed under AGPLv3 and JUCE's commercial licences. Distributing binaries of this plugin requires complying with one of them. pocketfft (BSD-3) and doctest (MIT) are also fetched at build time.
