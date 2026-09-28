"""Phase 3 review plots: loudness compensation and level tracking.

    python make_phase3_plots.py     # writes plots/12-14_*.png

Reference 95 dB(C) at the mix position, calibrated so the show starts at the
reference. The simulated show is synthetic band music: 40 s songs with 6 s
pauses, the engineer pulling the level down 12 dB at 2:30 and back up 6 dB
at 3:50, and a crowd at the mix position for the mic-tracking comparison.
"""

from __future__ import annotations

import numpy as np

import make_plots as mp
from make_plots import AXIS, BLUE, CONTEXT, INK, INK2, MUTED, ORANGE, WASH, freq_axis, plt, save
from roomeq import filters, iso226, loudness, roomsim
from roomeq.filters import Band

FS = 48000
REF = 95.0
VIOLET = "#8f63d9"
MAGENTA = "#c2447f"
CFG = loudness.LoudnessConfig()
PLAN = loudness.plan_shelves(REF, FS)


def stage_bands(low, high, hp_base=None, cfg=CFG):
    return loudness.loudness_bands(PLAN, low, high, cfg, hp_base)


def plot_contours_and_shelves():
    f = mp.GRID
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(15, 4.8), gridspec_kw={"wspace": 0.16})

    shades = ["#9ec5f2", "#5f9ee8", BLUE, "#16427f"]
    for phon, color in zip((65, 75, 85, 95), shades):
        ax.plot(f, iso226.relative_contour(f, phon), color=color, lw=2.0, label=f"{phon} phon")
    ax.set_ylim(-12, 45)
    ax.set_ylabel("Level needed re 1 kHz (dB)")
    ax.set_title("ISO 226 equal-loudness contours (normalised at 1 kHz)", fontsize=11)
    ax.legend(loc="upper right")
    freq_axis(ax)

    ax2.axhline(0, color=AXIS, lw=0.8)
    ax2.axhspan(-20, 0, color=WASH, lw=0, zorder=0)
    for drop, color in zip((5, 10, 15, 20), shades):
        target = loudness.compensation_target(f, REF - drop, REF)
        low = float(np.interp(drop, PLAN.deltas, PLAN.low_gain))
        high = float(np.interp(drop, PLAN.deltas, PLAN.high_gain))
        ax2.plot(f, target, color=color, lw=2.0, label=f"{drop} dB below: ISO 226 difference")
        ax2.plot(f, filters.response_db(stage_bands(low, high), f, FS), color=color, lw=1.2, ls=(0, (4, 3)))
    low, high = loudness.shelf_gains(PLAN, REF - 20, CFG)
    ax2.plot(f, filters.response_db(stage_bands(low, high), f, FS), color=ORANGE, lw=2.6,
             label=f"20 dB below, as applied (max {CFG.max_low_db:g} / {CFG.max_high_db:g} dB)")
    ax2.set_ylim(-2, 12)
    ax2.set_ylabel("Boost (dB)")
    ax2.set_title(f"Compensation below a {REF:g} dB(C) reference: target (solid) vs shelves (dashed)", fontsize=11)
    ax2.legend(loc="upper right", fontsize=8)
    freq_axis(ax2)
    fig.text(0.01, -0.08, f"Shelves fitted once per reference: low shelf {PLAN.low_freq:.0f} Hz (Q {PLAN.low_q:g}), "
             f"high shelf {PLAN.high_freq / 1000:.1f} kHz; only their gains follow the level. They stay within 0.8 dB "
             "of the ISO 226 difference\nfrom 30 Hz to 16 kHz for drops up to 20 dB. The small 2-4 kHz dip in the "
             "difference (under 1 dB) is left alone: shelves only boost.", color=MUTED, fontsize=8.5)
    save(fig, "12_loudness_compensation.png")


def simulated_show(rng):
    """(program, fader dB per sample, song spans)."""
    song = roomsim.synthetic_program(40.0, FS, rng, level_dbfs=-18.0)
    events = [(0.0, 0.0), (150.0, -12.0), (230.0, -6.0)]
    parts, spans, t = [], [], 0.0
    for k in range(7):
        n = len(song)
        tt = t + np.arange(n) / FS
        g = np.select([tt >= e[0] for e in events[::-1]], [e[1] for e in events[::-1]], 0.0)
        parts.append(song * 10 ** ((rng.uniform(-1.5, 1.5) + g) / 20))
        spans.append((t, t + 40.0))
        t += 40.0
        parts.append(np.zeros(int(6 * FS)))
        t += 6.0
    return np.concatenate(parts), spans, events


def plot_tracking():
    rng = np.random.default_rng(12)
    x, spans, events = simulated_show(rng)
    crowd = roomsim.make_noise(len(x), FS, roomsim.NoiseSpec(pink_dbfs=None, babble_dbfs=-42.0), rng)
    mic = 10 ** (-12 / 20) * x + crowd          # the mic hears the program 12 dB down, plus the crowd

    cal_out = loudness.c_weighted_level_dbfs(x[int(2 * FS):int(38 * FS)], FS)    # first song = reference level
    cal_mic = loudness.c_weighted_level_dbfs(10 ** (-12 / 20) * x[int(2 * FS):int(38 * FS)], FS)
    cal = loudness.Calibration(output_dbfs=cal_out, spl=REF, mic_dbfs=cal_mic)

    out_tr, mic_tr = loudness.LevelTracker(FS, CFG), loudness.LevelTracker(FS, CFG)
    deadband = loudness.Deadband(CFG)
    held_level = None
    y = loudness.ss.sosfilt(loudness.c_weighting_sos(FS), x)
    block = 512
    times, est_out, est_mic, momentary, lows, highs, held = [], [], [], [], [], [], []
    for i in range(0, len(x) - block + 1, block):
        out_tr.process(x[i:i + block])
        mic_tr.process(mic[i:i + block], follow=out_tr)
        if out_tr.count < block:                   # a window just completed
            held_level = deadband.update(out_tr.estimate)
        if (i // block) % 20 == 0:
            times.append(i / FS)
            spl = cal.spl_from_output(out_tr.estimate) if out_tr.estimate is not None else np.nan
            est_out.append(spl)
            held_spl = cal.spl_from_output(held_level) if held_level is not None else np.nan
            held.append(held_spl)
            est_mic.append(cal.spl_from_mic(mic_tr.estimate) if mic_tr.estimate is not None else np.nan)
            seg = y[max(0, i - int(0.4 * FS)):i + 1]
            momentary.append(cal.spl_from_output(10 * np.log10(np.mean(seg ** 2) + 1e-30)) if len(seg) > 10 else np.nan)
            low, high = loudness.shelf_gains(PLAN, held_spl, CFG) if np.isfinite(held_spl) else (0.0, 0.0)
            lows.append(low)
            highs.append(high)
    times = np.array(times) / 60.0

    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(12, 7), sharex=True, gridspec_kw={"height_ratios": [2.2, 1.2], "hspace": 0.08})
    for a, b in spans:
        ax.axvspan(a / 60, b / 60, color=WASH, lw=0, zorder=0)
    true_leq = []
    for a, b in spans:
        seg = y[int(a * FS):int(b * FS)]
        true_leq.append(cal.spl_from_output(10 * np.log10(np.mean(seg ** 2))))
        ax.plot([a / 60, b / 60], [true_leq[-1]] * 2, color=INK, lw=1.2, ls=(0, (5, 3)), zorder=2)
    ax.plot(times, np.clip(momentary, 40, None), color=CONTEXT, lw=0.8, label="Momentary level (400 ms)", zorder=1)
    ax.plot(times, est_out, color=BLUE, lw=2.4, label="Tracked from the plugin's output", zorder=4)
    ax.plot(times, est_mic, color=VIOLET, lw=1.6, label="Tracked from the mic (crowd noise included)", zorder=3)
    ax.plot(times, held, color=INK, lw=1.6, label="Level the compensation uses (2 dB deadband)", zorder=5)
    ax.axhline(REF, color=ORANGE, lw=1.2, label=f"Reference {REF:g} dB(C)")
    ax.plot([], [], color=INK, lw=1.2, ls=(0, (5, 3)), label="Each song's true Leq")
    for t0, g in events[1:]:
        ax.annotate(f"fader {g:+g} dB" if t0 < 200 else "fader +6 dB", (t0 / 60, REF + 3), color=INK2, fontsize=9, ha="center")
        ax.axvline(t0 / 60, color=MUTED, lw=0.8, ls=(0, (2, 2)))
    ax.set_ylim(70, 100)
    ax.set_ylabel("SPL at the mix position (dB C)")
    ax.legend(loc="lower left", fontsize=8, ncol=2)
    ax.set_title("Level tracking through a show: holds in the pauses, backs off fast, grows back slowly", fontsize=11)

    ax2.plot(times, lows, color=BLUE, lw=2.2, label=f"Low shelf ({PLAN.low_freq:.0f} Hz)")
    ax2.plot(times, highs, color=MAGENTA, lw=2.0, label=f"High shelf ({PLAN.high_freq / 1000:.1f} kHz)")
    ax2.axhline(CFG.max_low_db, color=BLUE, lw=0.8, ls=(0, (2, 2)))
    ax2.axhline(CFG.max_high_db, color=MAGENTA, lw=0.8, ls=(0, (2, 2)))
    ax2.set_ylim(-0.5, 10)
    ax2.set_ylabel("Boost (dB)")
    ax2.set_xlabel("Time (minutes)")
    ax2.legend(loc="upper left", fontsize=8)
    fig.text(0.01, -0.03, "Grey bands: songs; gaps: 6 s pauses. Calibrated so the first song is the reference. The boost follows the "
             "deadband level: swells within 2 dB don't move it.\nAfter the "
             "12 dB pull-down most of the boost arrives within ~15 s and the last dB over the next minute; after the push back up it "
             "backs off within ~2 s.\nThe mic "
             "estimate only updates while the program plays, but the crowd doesn't turn down with the PA: it reads high, most "
             "at low show levels\n(about +3.5 dB after the pull-down here), which means less boost. Output tracking has no such bias.",
             color=MUTED, fontsize=8.5)
    save(fig, "13_level_tracking.png")
    return true_leq, est_out, times


def plot_limits_and_highpass():
    f = mp.GRID
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(15, 4.6), gridspec_kw={"wspace": 0.16})
    hp_cfg = loudness.LoudnessConfig(hp_track=True)
    base = 45.0
    shades = ["#9ec5f2", "#5f9ee8", BLUE, "#16427f"]
    ax.axhline(0, color=AXIS, lw=0.8)
    for drop, color in zip((5, 10, 15, 20), shades):
        low, high = loudness.shelf_gains(PLAN, REF - drop, hp_cfg)
        bands = loudness.loudness_bands(PLAN, low, high, hp_cfg, base)
        ax.plot(f, filters.response_db(bands, f, FS), color=color, lw=2.0,
                label=f"{drop} dB below: HP at {bands[2].freq:.0f} Hz")
    ax.axvline(base, color=MUTED, lw=0.8, ls=(0, (2, 2)))
    ax.annotate("PA roll-off (measured)", (base * 1.08, -12), color=INK2, fontsize=8.5, ha="left")
    ax.set_ylim(-24, 10)
    ax.set_ylabel("Loudness stage (dB)")
    ax.set_title("Protective high-pass: rises with the low boost", fontsize=11)
    ax.legend(loc="upper right", fontsize=8)
    freq_axis(ax)

    drops = np.linspace(0, 40, 161)
    fitted_low = np.interp(drops, PLAN.deltas, PLAN.low_gain)
    fitted_high = np.interp(drops, PLAN.deltas, PLAN.high_gain)
    applied = np.array([loudness.shelf_gains(PLAN, REF - d, CFG) for d in drops])
    gentle = np.array([loudness.shelf_gains(PLAN, REF - d, loudness.LoudnessConfig(amount=0.7)) for d in drops])
    ax2.plot(drops, fitted_low, color=CONTEXT, lw=1.4, label="Low, unlimited (ISO 226)")
    ax2.plot(drops, fitted_high, color=CONTEXT, lw=1.4, ls=(0, (4, 3)), label="High, unlimited")
    ax2.plot(drops, applied[:, 0], color=BLUE, lw=2.4, label=f"Low as applied (max {CFG.max_low_db:g} dB)")
    ax2.plot(drops, applied[:, 1], color=MAGENTA, lw=2.2, label=f"High as applied (max {CFG.max_high_db:g} dB)")
    ax2.plot(drops, gentle[:, 0], color=BLUE, lw=1.2, ls=(0, (2, 2)), label="Low at 70% amount")
    ax2.axhline(CFG.low_ceiling_db, color=ORANGE, lw=1.0, ls=(0, (5, 3)))
    ax2.annotate(f"fixed ceiling {CFG.low_ceiling_db:g} dB (max boost can't exceed it)", (1, CFG.low_ceiling_db + 0.4),
                 color=ORANGE, fontsize=8.5)
    ax2.set_xlim(0, 40)
    ax2.set_ylim(0, 22)
    ax2.set_xlabel(f"dB below the {REF:g} dB reference")
    ax2.set_ylabel("Shelf gain (dB)")
    ax2.set_title("Gain against level, with amount and limits", fontsize=11)
    ax2.legend(loc="upper left", fontsize=8)
    save(fig, "14_loudness_limits.png")


def main():
    plot_contours_and_shelves()
    true_leq, est, times = plot_tracking()
    plot_limits_and_highpass()
    print(f"shelves for {REF:g} dB reference: low {PLAN.low_freq:.0f} Hz Q {PLAN.low_q:g}, high {PLAN.high_freq:.0f} Hz")
    for d in (5, 10, 15, 20):
        print(f"  {d:2d} dB below: low {np.interp(d, PLAN.deltas, PLAN.low_gain):.1f} dB, "
              f"high {np.interp(d, PLAN.deltas, PLAN.high_gain):.1f} dB (before limits)")
    print("song Leq (dB C):", " ".join(f"{v:.1f}" for v in true_leq))


if __name__ == "__main__":
    main()
