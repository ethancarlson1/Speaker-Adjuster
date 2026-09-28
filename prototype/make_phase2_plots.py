"""Phase 2 review plots: target curves, the correction fit, and re-measurement.

    python make_phase2_plots.py     # writes plots/07-11_*.png and prints the fitted bands

Same simulated club and load-in noise as the Phase 1 plots. "Re-measured"
means the sweep is played through the fitted correction filters (the real
biquads at 48 kHz) and captured again at the same five positions.
"""

from __future__ import annotations

import numpy as np
import scipy.signal as ss

import make_plots as mp
from make_plots import AXIS, BLUE, BLUE_WASH, CONTEXT, INK, INK2, MUTED, ORANGE, WASH, freq_axis, plt, save
from roomeq import averaging, capture, correction, filters, roomsim, spectrum, targets
from roomeq.filters import Band

FS = mp.FS
RED_WASH = "#f6d5d5"
VOICING = [Band(filters.HIGH_PASS, 35.0, q=0.7071), Band(filters.LOW_SHELF, 90.0, 2.0),
           Band(filters.BELL, 400.0, -1.5, 1.4)]


def measure(sim, seed, positions=range(5), prefilter=None):
    rng = np.random.default_rng(seed)
    play = mp.PLAY if prefilter is None else ss.sosfilt(prefilter, mp.PLAY)
    return [capture.analyze_sweep_capture(f"P{p + 1}", [sim.play(play, p, mp.LOADIN, rng) for _ in range(2)], mp.CFG)
            for p in positions]


def shade_regions(ax, res, nulls=True):
    lo, hi = res.fit_range
    ax.axvspan(20, lo, color=WASH, lw=0, zorder=0)
    if hi < 19999:
        ax.axvspan(hi, 20000, color=WASH, lw=0, zorder=0)
    if nulls:      # only where it matters: inside the fit range
        mask = res.null_mask & (res.grid >= lo) & (res.grid <= hi)
        edges = np.flatnonzero(np.diff(np.concatenate([[0], mask.astype(int), [0]])))
        for a, b in zip(edges[::2], edges[1::2]):
            ax.axvspan(res.grid[a], res.grid[b - 1], color=RED_WASH, lw=0, zorder=0)


def band_label(b: Band) -> str:
    f = f"{b.freq / 1000:.1f}k" if b.freq >= 1000 else f"{b.freq:.0f}"
    if b.kind == filters.BELL:
        return f"{f} {b.gain_db:+.1f} dB, {filters.bandwidth_for_q(b.q):.2f} oct"
    return f"{'low' if b.kind == filters.LOW_SHELF else 'high'} shelf {f} {b.gain_db:+.1f} dB"


# ---------------------------------------------------------------------------

def plot_targets():
    f = mp.GRID
    fig, ax = plt.subplots(figsize=(10, 3.8))
    ax.axhline(0, color=AXIS, lw=0.8)
    for t, color, lw in ((targets.FLAT, CONTEXT, 2.0), (targets.HOUSE, BLUE, 2.4), (targets.SPEECH, ORANGE, 2.4)):
        ax.plot(f, t.db(f), color=color, lw=lw, label=t.name)
        if len(t.points) > 1:
            ax.plot([p[0] for p in t.points], [p[1] for p in t.points], "o", color=color, ms=3.5)
    ax.set_ylim(-13, 6)
    ax.set_ylabel("Target (dB, relative)")
    ax.legend(loc="lower right", ncol=3)
    ax.set_title("Target presets (dots are the editable points; custom curves work the same way)")
    freq_axis(ax)
    fig.text(0.01, -0.12, "House: +4 dB below ~80 Hz easing to 0 by 250 Hz, −1 dB/octave above 2 kHz (−3 dB at 16 kHz).  "
             "Speech: −6 dB at 75 Hz, +2 dB at 2–4 kHz, −3 dB at 16 kHz.\nThe curve is placed on the measured average by its "
             "250 Hz–4 kHz mean, so correction reshapes the ends rather than moving the overall level.",
             color=MUTED, fontsize=8.5)
    save(fig, "07_targets.png")


def plot_fit(caps, summary, res, target):
    g = res.grid
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 7.4), sharex=True,
                                  gridspec_kw={"height_ratios": [2.2, 1.6], "hspace": 0.08})
    shade_regions(ax, res)
    for i, c in enumerate(caps):
        off = summary.offsets_db[i]
        ax.plot(mp.GRID, mp.sdb(c.freqs, c.power * 10 ** (off / 10), 6, c.weight), color=CONTEXT, lw=0.9,
                label="Each position" if i == 0 else None, zorder=1)
    ax.plot(g, res.target_db, color=INK, lw=1.3, ls=(0, (5, 3)), label=f"Target ({target.name.lower()})", zorder=2)
    ax.plot(g, res.average_db, color=BLUE, lw=2.4, label="Measured average", zorder=3)
    ax.plot(g, res.predicted_db, color=ORANGE, lw=2.0, label="Predicted with correction", zorder=4)
    level = targets.anchor_offset_db(g, res.average_db, target)
    ax.set_ylim(level - 22, level + 10)
    ax.set_ylabel("Level (dB, 1/6-oct)")
    ax.legend(loc="lower center", ncol=4)
    ax.set_title(f"Correction fit: {len(res.bands)} bands, predicted {res.rms_error_db:.2f} dB RMS from target "
                 "in the corrected range")

    shade_regions(ax2, res)
    ax2.axhline(0, color=AXIS, lw=0.8)
    lim = dict(color=MUTED, lw=0.9, ls=(0, (2, 2)))
    ax2.plot(g, np.where(res.upper_db > 0, res.upper_db, np.nan), **lim)
    ax2.plot(g, res.lower_db, **lim)
    ax2.text(21, correction.CorrectionConfig().max_boost_db + 0.4, "max boost", color=MUTED, fontsize=8)
    ax2.text(21, -correction.CorrectionConfig().max_cut_db + 0.4, "max cut", color=MUTED, fontsize=8)
    ax2.plot(g, np.where(res.desired_db != 0, res.desired_db, np.nan), color=INK2, lw=1.0, ls=(0, (1, 1.5)),
             label="Wanted (target − average, clipped)")
    for b in res.bands:
        ax2.plot(g, filters.band_db(b, g, FS), color=BLUE_WASH, lw=1.2, zorder=1)
    ax2.plot(g, res.correction_db, color=BLUE, lw=2.4, label="Correction (sum of bands)", zorder=3)
    for b in res.bands:
        y = float(filters.band_db(b, np.array([b.freq]), FS)[0])
        ax2.plot(b.freq, y, "o", color=BLUE, ms=4, zorder=4)
    ax2.set_ylim(-14, 6)
    ax2.set_ylabel("EQ (dB)")
    ax2.legend(loc="lower right", ncol=1)
    freq_axis(ax2)
    listing = "   ".join(band_label(b) for b in res.bands)
    fig.text(0.01, -0.02, f"Bands: {listing}", color=INK2, fontsize=8, wrap=True)
    fig.text(0.01, -0.075, f"Grey: outside the fit range ({res.fit_range[0]:.0f} Hz to {res.fit_range[1] / 1000:.1f} kHz, "
             "the PA's −6 dB points). Red: nulls (average > 6 dB below its 1-octave trend, or positions disagreeing\n"
             "by > 6 dB at 1/3 octave), where the correction may cut but never boost. Boosts ≥ 1 octave wide; cuts ≥ 1/3 octave "
             "below ~300 Hz, ≥ 2/3 octave above.", color=MUTED, fontsize=8.5)
    save(fig, "08_correction_fit.png")


def plot_verify(sim, caps, summary):
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.4), sharey=True, gridspec_kw={"wspace": 0.06})
    rows = []
    for ax, target, seed in zip(axes, targets.PRESETS, (21, 22, 23)):
        res = correction.design_correction(caps, summary, target, FS)
        after = averaging.summarize_session(measure(sim, seed, prefilter=filters.sos(res.bands, FS)))
        g = res.grid

        def third(s):
            level = spectrum.to_db(spectrum.smooth_power(s.freqs, s.power, 3, g, s.weight))
            return level - targets.anchor_offset_db(g, level, target)

        b, a = third(summary), third(after)
        t = target.db(g)
        reachable = res.target_db - res.average_db <= correction.CorrectionConfig().max_boost_db + 1.0
        judge = (g >= res.fit_range[0]) & (g <= res.fit_range[1]) & ~res.null_mask & reachable
        rms_b = np.sqrt(np.mean((b - t)[judge] ** 2))
        rms_a = np.sqrt(np.mean((a - t)[judge] ** 2))
        worst = np.max(np.abs(a - t)[judge])
        shade_regions(ax, res)
        ax.fill_between(g, np.where(judge, t - 3, np.nan), np.where(judge, t + 3, np.nan), color=BLUE_WASH, lw=0,
                        alpha=0.6, label="Target ± 3 dB (judged range)")
        ax.plot(g, t, color=INK, lw=1.2, ls=(0, (5, 3)), label="Target")
        ax.plot(g, b, color=CONTEXT, lw=1.6, label="Before")
        ax.plot(g, a, color=BLUE, lw=2.2, label="Re-measured with correction")
        ax.set_ylim(-18, 9)
        ax.set_title(f"{target.name}: RMS {rms_b:.1f} → {rms_a:.1f} dB, worst {worst:.1f} dB", fontsize=11)
        freq_axis(ax)
        rows.append((target.name, len(res.bands), rms_b, rms_a, worst))
    axes[0].set_ylabel("Level re target (dB, 1/3-oct)")
    axes[0].legend(loc="lower center", fontsize=8)
    fig.text(0.01, -0.07, "Five positions, two 5 s sweeps each, in load-in noise; the correction is fitted from one session and "
             "checked with a fresh one played through the correction filters.\nJudged: the fit range, outside nulls (red), "
             "where the target is reachable within the +3 dB boost limit (the flat target's top half-octave is not).",
             color=MUTED, fontsize=8.5)
    save(fig, "09_remeasured.png")
    return rows


def plot_display(summary, res, target):
    """The single graph the spec asks for: measurement, target, correction, voicing, combined result."""
    g = res.grid
    voicing = filters.response_db(VOICING, g, FS)
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 6.6), sharex=True,
                                  gridspec_kw={"height_ratios": [2.2, 1.2], "hspace": 0.08})
    shade_regions(ax, res, nulls=False)
    ax.plot(g, res.target_db, color=INK, lw=1.3, ls=(0, (5, 3)), label=f"Target ({target.name.lower()})")
    ax.plot(g, res.average_db, color=CONTEXT, lw=2.0, label="Measured")
    ax.plot(g, res.average_db + res.correction_db + voicing, color=BLUE, lw=2.4, label="Predicted (correction + voicing)")
    level = targets.anchor_offset_db(g, res.average_db, target)
    ax.set_ylim(level - 22, level + 10)
    ax.set_ylabel("Level (dB)")
    ax.legend(loc="lower center", ncol=3)
    ax.set_title("Proposed plugin graph: response on top, the two EQ stages below")

    ax2.axhline(0, color=AXIS, lw=0.8)
    ax2.plot(g, res.correction_db, color=BLUE, lw=2.0, label="Correction (measured, re-fit on each measurement)")
    ax2.plot(g, voicing, color=ORANGE, lw=2.0, label="Voicing EQ (engineer's, never overwritten)")
    ax2.plot(g, res.correction_db + voicing, color=INK, lw=1.2, ls=(0, (4, 2)), label="Combined")
    for b in VOICING:
        if b.kind in (filters.BELL, filters.LOW_SHELF, filters.HIGH_SHELF):
            ax2.plot(b.freq, float(filters.band_db(b, np.array([b.freq]), FS)[0]), "o", color=ORANGE, ms=6, mfc="white",
                     mew=1.8)
    ax2.set_ylim(-14, 6)
    ax2.set_ylabel("EQ (dB)")
    ax2.legend(loc="lower right", fontsize=8)
    freq_axis(ax2)
    fig.text(0.01, -0.04, "Example voicing: 35 Hz high-pass, +2 dB low shelf at 90 Hz, −1.5 dB at 400 Hz. Open circles are "
             "draggable handles in the plugin. The correction shown is the proposed one; 'Apply' puts it on the audio path.",
             color=MUTED, fontsize=8.5)
    save(fig, "10_display.png")


def plot_quick_and_notch(caps, notch_caps):
    fig, axes = plt.subplots(1, 2, figsize=(15, 4.6), gridspec_kw={"wspace": 0.12})

    one = averaging.summarize_session(caps[:1])
    res = correction.design_correction(caps[:1], one, targets.FLAT, FS)
    full = filters.response_db(res.fitted, res.grid, FS)
    ax = axes[0]
    shade_regions(ax, res)
    ax.axhline(0, color=AXIS, lw=0.8)
    ax.axhspan(-3, 3, color=WASH, lw=0, zorder=0)
    ax.plot(res.grid, np.where(res.desired_db != 0, res.desired_db, np.nan), color=INK2, lw=1.0, ls=(0, (1, 1.5)),
            label="Wanted")
    ax.plot(res.grid, full, color=CONTEXT, lw=1.6, label="Fitted at full strength")
    ax.plot(res.grid, res.correction_db, color=BLUE, lw=2.4, label="Applied: 50%, within ±3 dB")
    ax.set_ylim(-10, 6)
    ax.set_ylabel("EQ (dB)")
    ax.legend(loc="lower right", fontsize=8)
    ax.set_title("Quick mode: one good position (1/2-octave smoothing)", fontsize=11)
    freq_axis(ax)

    s = averaging.summarize_session(notch_caps)
    res = correction.design_correction(notch_caps, s, targets.FLAT, FS)
    ax = axes[1]
    shade_regions(ax, res)
    ax.plot(res.grid, res.target_db, color=INK, lw=1.2, ls=(0, (5, 3)), label="Target (flat)")
    ax.plot(res.grid, res.average_db, color=CONTEXT, lw=2.0, label="Measured (crossover notch at 1.4 kHz)")
    ax.plot(res.grid, res.predicted_db, color=BLUE, lw=2.2, label="Predicted with correction")
    ax.plot(res.grid, res.target_db + res.correction_db, color=ORANGE, lw=1.4,
            label="Correction (drawn on the target)")
    level = targets.anchor_offset_db(res.grid, res.average_db, targets.FLAT)
    ax.set_ylim(level - 24, level + 8)
    ax.legend(loc="lower right", fontsize=8)
    ax.set_title("A notch every position hears: cut around it, never boost into it", fontsize=11)
    freq_axis(ax)
    fig.text(0.01, -0.06, "Left: the fit uses limits raised to cap/strength, then every gain is halved, so the applied "
             "correction never exceeds the ±3 dB cap. Right: red = detected null.", color=MUTED, fontsize=8.5)
    save(fig, "11_quick_mode_and_nulls.png")


def main():
    sim = roomsim.SimulatedRoom()
    caps = measure(sim, 7)
    summary = averaging.summarize_session(caps)

    plot_targets()
    house = correction.design_correction(caps, summary, targets.HOUSE, FS)
    plot_fit(caps, summary, house, targets.HOUSE)
    rows = plot_verify(sim, caps, summary)
    plot_display(summary, house, targets.HOUSE)

    notched = roomsim.SimulatedRoom(pa=roomsim.PASpec(peq=roomsim.PASpec().peq + ((1400.0, -18.0, 5.0),)))
    plot_quick_and_notch(caps, measure(notched, 9, positions=range(3)))

    print(f"fit range {house.fit_range[0]:.0f} Hz - {house.fit_range[1]:.0f} Hz")
    for name, n, rb, ra, worst in rows:
        print(f"{name:7s} {n:2d} bands   RMS {rb:.2f} -> {ra:.2f} dB, worst {worst:.2f} dB after")
    print("house bands:")
    for b in house.bands:
        print("   ", band_label(b), f"(Q {b.q:.2f})" if b.kind == filters.BELL else "")


if __name__ == "__main__":
    main()
