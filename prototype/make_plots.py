"""Phase 1 review plots, generated from the simulated club.

    python make_plots.py            # writes plots/*.png and prints a summary

Scenario: 5 s sweeps, 2 per position, five audience positions, load-in
noise (pink background plus the odd bang). Extra captures show the grading
catching a truck idling, a dropped case, and walk-in music with a crowd.
"""

from __future__ import annotations

import textwrap
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.patches import Rectangle  # noqa: E402

from roomeq import averaging, capture, roomsim, spectrum, sweep  # noqa: E402
from roomeq.grading import Grade  # noqa: E402

OUT = Path(__file__).parent / "plots"

# Reference data-viz palette (light). Series: blue, orange. Context: greys.
SURFACE, INK, INK2, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#898781"
GRIDC, AXIS, CONTEXT, WASH = "#e1e0d9", "#c3c2b7", "#c3c2b7", "#f0efec"
BLUE, ORANGE = "#2a78d6", "#eb6834"
BLUE_WASH = "#cde2fb"
STATUS = {Grade.PASS: "#0ca30c", Grade.MARGINAL: "#fab219", Grade.REDO: "#d03b3b"}
SYMBOL = {Grade.PASS: "✓", Grade.MARGINAL: "!", Grade.REDO: "✗"}

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "axes.titlecolor": INK,
    "axes.titlesize": 12, "axes.titleweight": "bold", "axes.titlelocation": "left", "axes.titlepad": 10,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRIDC, "grid.linewidth": 0.6, "grid.linestyle": "-",
    "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelcolor": INK2, "ytick.labelcolor": INK2,
    "font.size": 10, "lines.linewidth": 2.0, "lines.solid_capstyle": "round",
    "legend.frameon": False, "legend.fontsize": 9, "legend.labelcolor": INK2,
})

CFG = sweep.SweepConfig(duration=5.0)
SWEEP = sweep.generate_sweep(CFG)
PLAY = sweep.build_playback(CFG, SWEEP)
FS = CFG.fs
GRID = spectrum.log_freq_grid(20, 20000, 48)
LOADIN = roomsim.NoiseSpec(pink_dbfs=-50.0, bursts=2, burst_dbfs=-25.0)
XTICKS = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]


def freq_axis(ax):
    ax.set_xscale("log")
    ax.set_xlim(20, 20000)
    ax.set_xticks(XTICKS)
    ax.set_xticklabels([f"{t // 1000}k" if t >= 1000 else str(t) for t in XTICKS])
    ax.minorticks_off()
    ax.set_xlabel("Frequency (Hz)")


def sdb(freqs, power, fraction=6, weights=None):
    return spectrum.to_db(spectrum.smooth_power(freqs, power, fraction, GRID, weights))


def session(sim, seed, positions=range(5), noise=LOADIN, repeats=2):
    rng = np.random.default_rng(seed)
    return [capture.analyze_sweep_capture(f"P{p + 1}", [sim.play(PLAY, p, noise, rng) for _ in range(repeats)],
                                          CFG, sweep=SWEEP) for p in positions]


def average(caps, align=True):
    weights = [averaging.band_mask_weights(c.freqs, c.grade) * c.weight for c in caps]
    return averaging.power_average(caps[0].freqs, [c.power for c in caps], weights, align_levels=align)


def truth_average(sim, positions=range(5)):
    powers = [capture.windowed_response(sim.linear_ir(p), FS)[1] for p in positions]
    freqs = capture.windowed_response(sim.linear_ir(0), FS)[0]
    return averaging.power_average(freqs, powers, [capture.in_band(freqs, 20, 20000)] * len(powers))


def error_line(ax, values, usable):
    """Blue inside the PA's usable range, grey outside it (where nothing is corrected)."""
    inside = (GRID >= usable[0]) & (GRID <= usable[1])
    ax.plot(GRID, np.where(inside, np.nan, values), color=CONTEXT, lw=1.2)
    ax.plot(GRID, np.where(inside, values, np.nan), color=BLUE, lw=1.6)


def save(fig, name):
    OUT.mkdir(exist_ok=True)
    fig.savefig(OUT / name, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ---------------------------------------------------------------------------

def plot_impulse_response(sim_distorted):
    rng = np.random.default_rng(1)
    rec = sim_distorted.play(PLAY, 0, roomsim.NoiseSpec(), rng)
    dec = sweep.deconvolve(rec, SWEEP, FS, CFG.f1, CFG.f2)
    arrival = sweep.find_arrival(dec, CFG.n_preroll)

    block = int(0.002 * FS)
    lo, hi = arrival - int(1.2 * FS), dec.zero + CFG.n_preroll + CFG.n_tail
    seg = dec.h[lo:hi]
    n = len(seg) // block
    env = 10 * np.log10(np.mean(seg[:n * block].reshape(n, block) ** 2, axis=1) + 1e-20)
    env -= env.max()
    t = (lo - arrival + (np.arange(n) + 0.5) * block) / FS

    fig, ax = plt.subplots(figsize=(10, 4.2))
    win = capture.AnalysisConfig().window
    noise_end = (dec.zero + CFG.n_preroll + CFG.n_tail - int(0.05 * FS) - arrival) / FS
    noise_start = noise_end - (win.pre + win.post)
    ax.axvspan(-win.pre, win.post, color=BLUE_WASH, lw=0, zorder=0)
    ax.axvspan(noise_start, noise_end, color=WASH, lw=0, zorder=0)
    ax.plot(t, env, color=BLUE, lw=1.2, zorder=2)
    ax.text(win.post / 2 - win.pre / 2, 3, "analysis window\n−50 ms … +500 ms", ha="center", va="bottom", color=INK2, fontsize=9)
    ax.text((noise_start + noise_end) / 2, 3, "noise estimate\n(same window, IR tail)", ha="center", va="bottom", color=INK2, fontsize=9)
    for k in (2, 3):
        tk = -CFG.rate * np.log(k)
        i = np.argmin(np.abs(t - tk))
        ax.annotate(f"{k}{'nd' if k == 2 else 'rd'} harmonic", (tk, env[i]), (tk - 0.08, env[i] + 22),
                    color=INK2, fontsize=9, ha="center", arrowprops=dict(arrowstyle="-", color=MUTED, lw=0.8))
    ax.set_xlim(t[0], t[-1])
    ax.set_ylim(-110, 12)
    ax.set_xlabel("Time relative to main arrival (s)")
    ax.set_ylabel("Level (dB re peak, 2 ms RMS)")
    ax.set_title("Deconvolved impulse response: distortion lands before the arrival and is windowed out")
    fig.text(0.01, -0.04, f"5 s sweep, PA driven to −30 dB 2nd / −35 dB 3rd harmonic, position P1. "
             f"Detected loop delay {1000 * (arrival - dec.zero - CFG.n_preroll) / FS:.2f} ms "
             f"(6 ms latency + {sim_distorted.distance(0):.1f} m flight).", color=MUTED, fontsize=8.5)
    save(fig, "01_impulse_response.png")


def plot_average_vs_truth(sim, caps):
    avg = average(caps)
    truth = truth_average(sim)
    a_db, t_db = sdb(avg.freqs, avg.power, 6, avg.weight), sdb(truth.freqs, truth.power, 6, truth.weight)
    lo_hz, hi_hz = averaging.usable_range(GRID, sdb(avg.freqs, avg.power, 1, avg.weight))
    level = averaging.target_level_db(GRID, a_db)

    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 6.6), sharex=True, gridspec_kw={"height_ratios": [3, 1.1], "hspace": 0.08})
    for i, c in enumerate(caps):
        off = avg.offsets_db[i]
        ax.plot(GRID, sdb(c.freqs, c.power * 10 ** (off / 10), 6, c.weight), color=CONTEXT, lw=1.0,
                label="Each position (level-aligned)" if i == 0 else None, zorder=1)
    ax.axvspan(20, lo_hz, color=WASH, lw=0, zorder=0)
    ax.axvspan(hi_hz, 20000, color=WASH, lw=0, zorder=0)
    ax.plot([lo_hz, hi_hz], [level, level], color=INK, lw=1.2, ls=(0, (5, 3)), label="Target (flat), over usable range", zorder=2)
    ax.plot(GRID, a_db, color=BLUE, lw=2.6, label="Measured power average", zorder=3)
    ax.plot(GRID, t_db, color=ORANGE, lw=1.0, label="True average (simulator ground truth)", zorder=4)
    ax.set_ylim(level - 20, level + 12)
    ax.set_ylabel("Level (dB, 1/6-oct)")
    ax.legend(loc="lower center", ncol=2)
    ax.set_title("Five-position average vs ground truth, measured in load-in noise")

    err = a_db - t_db
    band = (GRID >= lo_hz) & (GRID <= hi_hz)
    ax2.axhspan(-1, 1, color=WASH, lw=0)
    ax2.axhline(0, color=AXIS, lw=0.8)
    error_line(ax2, err, (lo_hz, hi_hz))
    ax2.set_ylim(-2.5, 2.5)
    ax2.set_ylabel("Measured − true (dB)")
    ax2.text(25, 1.4, f"max |error| {np.nanmax(np.abs(err[band])):.2f} dB in usable range", color=INK2, fontsize=9)
    freq_axis(ax2)
    fig.text(0.01, 0.0, "Positions 5–14 m from the PA, off the centre line; two 5 s sweeps each, pink noise at −50 dBFS plus two "
             f"random bangs per sweep.\nShaded: below the PA's −10 dB point ({lo_hz:.0f} Hz), where no target is drawn. "
             "Error band ±1 dB; grey = outside the usable range.", color=MUTED, fontsize=8.5)
    save(fig, "02_average_vs_truth.png")
    return avg, (lo_hz, hi_hz), float(np.nanmax(np.abs(err[band])))


def plot_grading(rows):
    n_rows, n_bands = len(rows), len(spectrum.OCTAVE_NOMINAL)
    fig = plt.figure(figsize=(15, 0.62 * n_rows + 1.6))
    ax = fig.add_axes([0.13, 0.02, 0.40, 0.80])
    ax.set_xlim(0, n_bands)
    ax.set_ylim(n_rows, 0)
    ax.axis("off")
    for j, name in enumerate(spectrum.OCTAVE_NOMINAL):
        ax.text(j + 0.5, -0.25, name, ha="center", va="bottom", color=INK2, fontsize=9)
    ax.text(-0.15, -0.25, "Octave band (Hz)", ha="right", va="bottom", color=MUTED, fontsize=9)
    for i, (label, cap) in enumerate(rows):
        fig.text(0.125, 0.82 - 0.80 * (i + 0.5) / n_rows, label, ha="right", va="center", color=INK, fontsize=9.5)
        for j, b in enumerate(cap.grade.bands):
            g = b.grade
            fill = WASH if g is None else STATUS[g]
            ax.add_patch(Rectangle((j + 0.04, i + 0.06), 0.92, 0.88, facecolor=fill, alpha=1.0 if g is None else 0.22, lw=0))
            if g is None:
                txt, color = "n/a", MUTED
            else:
                txt = f"{SYMBOL[g]} {b.snr_db:.0f}"
                if b.consistency_grade > Grade.PASS and b.consistency_grade >= b.snr_grade:
                    txt += f"\nΔ{b.spread_db:.1f}"
                color = INK
            ax.text(j + 0.5, i + 0.5, txt, ha="center", va="center", color=color, fontsize=8.5)
        overall = cap.grade.overall
        prefix = f"{overall.label}: "
        reasons = [r[len(prefix):] if r.startswith(prefix) else r for r in cap.grade.reasons] + cap.grade.notes
        text = f"{SYMBOL[overall]} {overall.label.upper()} — " + "; ".join(reasons)
        fig.text(0.545, 0.82 - 0.80 * (i + 0.5) / n_rows, "\n".join(textwrap.wrap(text, 78)), ha="left", va="center",
                 color=INK, fontsize=8.5)
    fig.text(0.01, 0.99, "Capture grading: per-band SNR and repeat consistency, with the reason shown to the engineer",
             ha="left", va="top", color=INK, fontsize=12, fontweight="bold")
    fig.text(0.01, 0.955, "Cell = SNR (dB). ✓ pass ≥ 20 dB SNR and repeats within 1 dB · ! marginal ≥ 10 dB / ≤ 3 dB · ✗ redo · "
             "Δ = repeat-to-repeat spread (dB) where it drives the grade · n/a = outside the PA's range, not graded",
             ha="left", va="top", color=MUTED, fontsize=9)
    save(fig, "03_capture_grading.png")


def plot_repeatability(sim):
    a = average(session(sim, 101))
    b = average(session(sim, 202))
    a_db, b_db = sdb(a.freqs, a.power, 6, a.weight), sdb(b.freqs, b.power, 6, b.weight)
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 6.2), sharex=True, gridspec_kw={"height_ratios": [3, 1.1], "hspace": 0.08})
    ax.plot(GRID, a_db, color=BLUE, lw=2.6, label="Session A")
    ax.plot(GRID, b_db, color=ORANGE, lw=1.0, label="Session B")
    usable = averaging.usable_range(GRID, sdb(a.freqs, a.power, 1, a.weight))
    level = averaging.target_level_db(GRID, a_db)
    ax.set_ylim(level - 20, level + 12)
    ax.set_ylabel("Level (dB, 1/6-oct)")
    ax.legend(loc="lower center", ncol=2)
    ax.set_title("Repeatability: two full 5-position sessions, independent noise and bangs")
    d = a_db - b_db
    ax2.axhspan(-0.5, 0.5, color=WASH, lw=0)
    ax2.axhline(0, color=AXIS, lw=0.8)
    error_line(ax2, d, usable)
    ax2.set_ylim(-1.5, 1.5)
    ax2.set_ylabel("A − B (dB)")
    sel = (GRID >= usable[0]) & (GRID <= usable[1])
    ax2.text(25, 0.8, f"max |A − B| {np.nanmax(np.abs(d[sel])):.2f} dB in the usable range ({usable[0]:.0f} Hz+); band ±0.5 dB",
             color=INK2, fontsize=9)
    freq_axis(ax2)
    save(fig, "04_repeatability.png")
    return float(np.nanmax(np.abs(d[sel]))), usable


def plot_smoothing_and_quick_mode(caps, avg):
    fig, (ax, ax2) = plt.subplots(1, 2, figsize=(14, 4.8), sharey=True, gridspec_kw={"wspace": 0.06})
    raw = sdb(avg.freqs, avg.power, 48, avg.weight)
    s6, s3 = sdb(avg.freqs, avg.power, 6, avg.weight), sdb(avg.freqs, avg.power, 3, avg.weight)
    level = averaging.target_level_db(GRID, s6)
    ax.plot(GRID, raw, color=CONTEXT, lw=1.0, label="1/48 octave (near raw)")
    ax.plot(GRID, s6, color=BLUE, lw=2.0, label="1/6 octave")
    ax.plot(GRID, s3, color=ORANGE, lw=1.6, label="1/3 octave")
    ax.set_ylim(level - 22, level + 14)
    ax.set_ylabel("Level (dB)")
    ax.legend(loc="lower center", ncol=3)
    ax.set_title("Smoothing choices on the 5-position average")
    freq_axis(ax)

    p1 = caps[0]
    policy = averaging.quick_mode_policy(1, 6)
    off = avg.offsets_db[0]
    ax2.plot(GRID, sdb(p1.freqs, p1.power * 10 ** (off / 10), 6, p1.weight), color=CONTEXT, lw=1.0, label="P1 alone, 1/6 octave")
    ax2.plot(GRID, s6, color=BLUE, lw=2.0, label="5-position average, 1/6 octave")
    ax2.plot(GRID, sdb(p1.freqs, p1.power * 10 ** (off / 10), policy.smoothing_fraction, p1.weight), color=ORANGE, lw=1.8,
             label=f"Quick mode: P1 at 1/{policy.smoothing_fraction} octave")
    ax2.legend(loc="lower center", ncol=1)
    ax2.set_title("Quick mode (one good position)")
    ax2.text(1000, level + 10, f"1 good position → 1/{policy.smoothing_fraction}-oct smoothing, "
             f"{policy.strength:.0%} strength, ±{policy.max_correction_db:.0f} dB cap\n"
             "2 → 1/3-oct, 75%, ±6 dB · 3+ → user smoothing, 100%", ha="center", color=INK2, fontsize=8.5)
    freq_axis(ax2)
    save(fig, "05_smoothing_and_quick_mode.png")


def plot_program_fallback(sim, prog, sweep_cap):
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(10, 6.4), sharex=True, gridspec_kw={"height_ratios": [2.4, 1.2], "hspace": 0.08})
    s_db = sdb(sweep_cap.freqs, sweep_cap.power, 6, sweep_cap.weight)
    p_db = sdb(prog.freqs, prog.power, 6, prog.weight)
    ax.plot(GRID, p_db, color=ORANGE, lw=1.6, label="Dual-FFT on walk-in music + crowd (30 s)")
    ax.plot(GRID, s_db, color=BLUE, lw=2.0, label="Sweep, same position")
    level = averaging.target_level_db(GRID, s_db)
    ax.set_ylim(level - 22, level + 12)
    ax.set_ylabel("Level (dB, 1/6-oct)")
    ax.legend(loc="lower center", ncol=2)
    ax.set_title("Program-material fallback vs sweep at P1")
    band = (GRID >= 80) & (GRID <= 16000)
    ax.text(25, level + 8, f"max difference {np.nanmax(np.abs((p_db - s_db)[band])):.2f} dB, 80 Hz–16 kHz", color=INK2, fontsize=9)

    # Band SNR from coherence, graded like a sweep capture.
    for b in prog.grade.bands:
        g = b.grade
        color = MUTED if g is None else STATUS[g]
        ax2.add_patch(Rectangle((b.lo, 0), b.hi - b.lo, b.snr_db, facecolor=color, alpha=0.35, lw=0))
        ax2.text(b.center, b.snr_db + 1.5, f"{SYMBOL.get(g, '')} {b.snr_db:.0f}", ha="center", color=INK, fontsize=8.5)
    ax2.axhline(20, color=AXIS, lw=0.8)
    ax2.axhline(10, color=AXIS, lw=0.8)
    ax2.set_yticks([0, 10, 20, 30, 40])
    ax2.set_yticklabels(["0", "marginal 10", "pass 20", "30", "40"])
    ax2.set_ylim(0, 45)
    ax2.set_ylabel("Band SNR (dB)\nfrom coherence")
    freq_axis(ax2)
    fig.text(0.01, 0.0, "Coherence sets the grade (crowd chatter pulls 250 Hz–4 kHz down) but does not weight the magnitude: "
             "coherence-weighting read 1–2 dB high in tests.", color=MUTED, fontsize=8.5)
    save(fig, "06_program_fallback.png")


def main():
    sim = roomsim.SimulatedRoom()
    distorted = roomsim.SimulatedRoom(pa=roomsim.PASpec(h2_db=-30.0, h3_db=-35.0))
    caps = session(sim, 7)

    plot_impulse_response(distorted)
    avg, usable, max_err = plot_average_vs_truth(sim, caps)

    rng = np.random.default_rng(8)
    rumble = capture.analyze_sweep_capture(
        "P3 (truck)", [sim.play(PLAY, 2, roomsim.NoiseSpec(rumble_dbfs=-25.0), rng) for _ in range(2)], CFG, sweep=SWEEP)
    bang = capture.analyze_sweep_capture(
        "P2 (bang)", [sim.play(PLAY, 1, roomsim.NoiseSpec(), rng),
                      sim.play(PLAY, 1, roomsim.NoiseSpec(bursts=1, burst_dbfs=0.0, burst_times=(2.0,)), rng)], CFG, sweep=SWEEP)
    quiet_1x = capture.analyze_sweep_capture(
        "P4 2 s ×1", [sim.play(sweep.build_playback(sweep.SweepConfig(duration=2.0)), 3,
                                roomsim.NoiseSpec(pink_dbfs=-40.0), rng)], sweep.SweepConfig(duration=2.0))
    x = roomsim.synthetic_program(30.0, FS, rng)
    prog = capture.analyze_program_capture("walk-in", x, sim.play(x, 0, roomsim.NoiseSpec(babble_dbfs=-40.0), rng), FS)

    rows = [(f"{c.name} · {sim.distance(i):.0f} m", c) for i, c in enumerate(caps)]
    rows += [("P3 · truck idling outside", rumble), ("P2 · case dropped in sweep 2", bang),
             ("P4 · one 2 s sweep, noisy", quiet_1x), ("P1 · walk-in music + crowd", prog)]
    plot_grading(rows)
    rep, rep_range = plot_repeatability(sim)
    plot_smoothing_and_quick_mode(caps, avg)
    plot_program_fallback(sim, prog, caps[0])

    print(f"usable range {usable[0]:.0f} Hz – {usable[1]:.0f} Hz")
    print(f"5-position average vs truth: max |error| {max_err:.2f} dB (1/6 oct, usable range)")
    print(f"session-to-session repeatability: max {rep:.2f} dB above {rep_range[0]:.0f} Hz")
    for label, c in rows:
        print(f"{label:32s} {c.grade.overall.label:9s} {'; '.join(c.grade.reasons + c.grade.notes)}  delay {c.delays_ms[0]:.2f} ms")


if __name__ == "__main__":
    main()
