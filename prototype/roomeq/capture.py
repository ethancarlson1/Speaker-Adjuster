"""One capture = one mic position: 1-3 sweeps (or a program excerpt) -> graded response."""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import dualfft
from .grading import CaptureGrade, GradingConfig, grade_capture, regrade
from .spectrum import WindowConfig, ir_window, log_freq_grid, rebin_power, response_nfft
from .sweep import SweepConfig, deconvolve, find_arrival, fractional_peak_offset, generate_sweep


@dataclass(frozen=True)
class AnalysisConfig:
    window: WindowConfig = WindowConfig()
    grading: GradingConfig = GradingConfig()
    max_delay_s: float = 1.0        # longest loop delay searched (latency + flight time)
    noise_margin_s: float = 0.05    # gap between noise window and end of valid tail


LF_GRID = log_freq_grid(20.0, 1000.0, 24)   # where a sweep keeps its complex response (sub alignment)


@dataclass
class Capture:
    name: str
    kind: str                        # "sweep" or "program"
    fs: float
    freqs: np.ndarray                # linear FFT grid
    power: np.ndarray                # |H|^2 (repeats coherently averaged)
    noise_power: np.ndarray          # noise estimate in |H|^2 units
    weight: np.ndarray               # per-bin 0/1: 0 outside the sweep band / where program had no energy
    grade: CaptureGrade
    repeat_powers: list[np.ndarray] = field(default_factory=list)
    delays_ms: list[float] = field(default_factory=list)
    ir: np.ndarray | None = None     # windowed, aligned, averaged IR (sweeps only)
    low: np.ndarray | None = None    # complex response on LF_GRID from the loop start (sweeps only)
    excluded: bool = False
    drift_ppm: float = float("nan")  # program captures: output/mic clock difference that was corrected (0 if none)


def low_frequency_response(h: np.ndarray, loop_start: int, arrival: int, fs: float,
                           window: WindowConfig = WindowConfig()) -> np.ndarray:
    """The complex response on LF_GRID of h from the loop start (time 0) to the
    end of the analysis window after the arrival, tapered like it.

    Everything the speaker sent is in it, however late the strongest peak: a
    sub's direct sound can come tens of ms before the room modes build up to
    the peak the analysis window is placed on. Time 0 is the loop start, so
    responses from the same interface can be summed as they are.
    """
    stop = min(len(h), arrival + int(round(window.post * fs)))
    x = np.asarray(h[loop_start:stop], dtype=float).copy()
    n_b = max(1, int(round(window.post * fs * window.taper_post)))
    n_b = min(n_b, len(x))
    x[len(x) - n_b:] *= 0.5 * (1 + np.cos(np.pi * np.arange(n_b) / n_b))
    n = np.arange(len(x))
    return np.array([np.dot(x, np.exp(-2j * np.pi * f * n / fs)) for f in LF_GRID])


def in_band(freqs: np.ndarray, f1: float, f2: float) -> np.ndarray:
    """Weight 1 inside the measured band, 0 outside. Keeps smoothing near
    20 Hz / 20 kHz from averaging in the band-limited roll-off."""
    return ((freqs >= f1) & (freqs <= f2)).astype(float)


def regrade_capture(c: Capture, grading: GradingConfig) -> None:
    """Grades a capture again against other settings (a new reference band),
    exactly as analysing it with them would have."""
    signal = c.power - c.noise_power if c.kind == "sweep" else c.power
    c.grade = regrade(c.grade, c.freqs, signal, grading)


def windowed_response(ir: np.ndarray, fs: float, cfg: AnalysisConfig = AnalysisConfig()) -> tuple[np.ndarray, np.ndarray, int]:
    """|H|^2 of a known IR (e.g. simulator ground truth) through exactly the
    window a capture uses, around its own peak. Returns (freqs, power, peak index)."""
    w, n_pre = ir_window(cfg.window, fs)
    nfft = response_nfft(cfg.window, fs)
    padded = np.concatenate([np.zeros(n_pre), ir, np.zeros(len(w))])
    pk = int(np.argmax(np.abs(padded)))
    power = np.abs(np.fft.rfft(padded[pk - n_pre:pk - n_pre + len(w)] * w, nfft)) ** 2
    return np.fft.rfftfreq(nfft, 1 / fs), power, pk - n_pre


def analyze_sweep_capture(name: str, recordings: list[np.ndarray], sweep_cfg: SweepConfig,
                          cfg: AnalysisConfig = AnalysisConfig(),
                          sweep: np.ndarray | None = None) -> Capture:
    """Deconvolve each repeat, line them up, average coherently, grade.

    Each recording is [preroll | sweep | tail] as produced by build_playback.
    """
    fs = sweep_cfg.fs
    if sweep is None:
        sweep = generate_sweep(sweep_cfg)
    w, n_pre = ir_window(cfg.window, fs)
    nfft = response_nfft(cfg.window, fs)
    freqs = np.fft.rfftfreq(nfft, 1 / fs)
    margin = int(round(cfg.noise_margin_s * fs))

    segments, noise_spectra, delays, lows = [], [], [], []
    for rec in recordings:
        dec = deconvolve(rec, sweep, fs, sweep_cfg.f1, sweep_cfg.f2)
        arrival = find_arrival(dec, sweep_cfg.n_preroll, cfg.max_delay_s)
        start = arrival - n_pre
        segments.append(dec.h[start:start + len(w)] * w)
        lows.append(low_frequency_response(dec.h, dec.zero + sweep_cfg.n_preroll, arrival, fs, cfg.window))

        # Noise: same window, as late as the tail allows. IR time t (from sweep
        # start) is valid at every frequency only up to t = tail.
        noise_end = dec.zero + sweep_cfg.n_preroll + sweep_cfg.n_tail - margin
        noise_start = noise_end - len(w)
        if noise_start < start + len(w):
            raise ValueError(f"{name}: tail too short for a {1000 * (arrival - dec.zero - sweep_cfg.n_preroll) / fs:.0f} ms "
                             f"loop delay; lengthen SweepConfig.tail")
        noise_spectra.append(np.abs(np.fft.rfft(dec.h[noise_start:noise_end] * w, nfft)) ** 2)
        delays.append(1000 * (arrival - dec.zero - sweep_cfg.n_preroll) / fs)

    # Line repeats up to sub-sample accuracy (integer part is already done by
    # peak alignment), then average coherently: SNR improves ~3 dB per doubling.
    spectra = []
    for i, seg in enumerate(segments):
        S = np.fft.rfft(seg, nfft)
        if i > 0:
            delta = fractional_peak_offset(segments[0], seg, max_lag=8)
            S = S * np.exp(2j * np.pi * freqs * delta / fs)
        spectra.append(S)
    n = len(spectra)
    H = np.mean(spectra, axis=0)
    power = np.abs(H) ** 2
    noise = np.sum(noise_spectra, axis=0) / n ** 2
    repeat_powers = [np.abs(S) ** 2 for S in spectra]

    grade = grade_capture(freqs, power - noise, noise, repeat_powers if n > 1 else None,
                          cfg.grading, f_max=min(sweep_cfg.f2, fs / 2))
    return Capture(name=name, kind="sweep", fs=fs, freqs=freqs, power=power, noise_power=noise,
                   weight=in_band(freqs, sweep_cfg.f1, sweep_cfg.f2), grade=grade, repeat_powers=repeat_powers,
                   delays_ms=delays, ir=np.fft.irfft(H, nfft)[:len(w)], low=np.mean(lows, axis=0))


def analyze_program_capture(name: str, reference: np.ndarray, mic: np.ndarray, fs: float,
                            cfg: AnalysisConfig = AnalysisConfig(),
                            dual_cfg: dualfft.DualFFTConfig = dualfft.DualFFTConfig()) -> Capture:
    """Dual-FFT estimate against program material, graded with the same rules.

    Coherence drives the grade (and so which bands get averaged), but it is
    NOT used to weight the magnitude: coherence is highest at the room's
    response peaks, so coherence-weighted smoothing reads ~1-2 dB high.
    Bins the program didn't excite at all are gated out instead.

    The dual-FFT frame is longer than the sweep window (to hold LF reverb);
    results are rebinned onto the sweep captures' grid so the two can be
    averaged together bin for bin. Clock drift between the output and the mic
    is measured and corrected first (see dualfft); the notes say so.
    """
    est = dualfft.transfer_function(reference, mic, fs, dual_cfg)
    freqs = np.fft.rfftfreq(response_nfft(cfg.window, fs), 1 / fs)
    power = rebin_power(est.freqs, np.abs(est.H) ** 2, freqs)
    noise = rebin_power(est.freqs, dualfft.noise_power_h_domain(est), freqs)
    weight = (rebin_power(est.freqs, dualfft.excitation_gate(est), freqs) > 0.5) * in_band(freqs, 20.0, 20000.0)
    grade = grade_capture(freqs, power, noise, None, cfg.grading, f_max=min(20000.0, fs / 2))
    grade.notes += dualfft.drift_notes(est.drift)
    return Capture(name=name, kind="program", fs=fs, freqs=freqs, power=power, noise_power=noise,
                   weight=weight, grade=grade, delays_ms=[1000 * est.delay / fs], drift_ppm=est.drift.ppm)
