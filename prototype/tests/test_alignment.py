"""Arrival times, their confidence, and the delay that lines a zone up."""
import numpy as np
import pytest

from roomeq import alignment, capture, roomsim, sweep

CFG = sweep.SweepConfig(duration=2.0)
PLAY = sweep.build_playback(CFG)
FS = CFG.fs


def fractional_impulse(delay_samples: float, n: int, taps: int = 64) -> np.ndarray:
    """A band-limited impulse at a fractional delay (windowed sinc)."""
    h = np.zeros(n)
    k0 = int(np.floor(delay_samples))
    idx = np.arange(k0 - taps, k0 + taps + 1)
    x = idx - delay_samples
    h[idx] = np.sinc(x) * np.kaiser(len(idx), 8.0)
    return h


def record(ir: np.ndarray, rng, noise=1e-5) -> np.ndarray:
    y = np.convolve(PLAY, ir)[:len(PLAY)]
    return y + noise * rng.standard_normal(len(y))


def test_a_clean_arrival_is_found_below_a_sample():
    rng = np.random.default_rng(1)
    for d in (480.0, 480.37, 1234.5):
        ir = fractional_impulse(d, 4096) + 0.3 * fractional_impulse(d + 600, 4096)   # a reflection 12.5 ms later
        c = capture.analyze_sweep_capture("P", [record(ir, rng), record(ir, rng)], CFG)
        a = alignment.estimate_arrival(c)
        assert a.ms == pytest.approx(1000 * d / FS, abs=0.004), d        # a fifth of a sample at 48 kHz
        assert a.confidence == alignment.HIGH, a.reasons


def test_the_simulated_room_arrives_when_it_should(sim):
    rng = np.random.default_rng(2)
    recs = [sim.play(PLAY, 1, roomsim.NoiseSpec(), rng) for _ in range(2)]
    c = capture.analyze_sweep_capture("P", recs, CFG)
    a = alignment.estimate_arrival(c)
    assert abs(a.ms - c.delays_ms[0]) < 0.03                              # refines, doesn't move it
    assert a.confidence in (alignment.HIGH, alignment.MEDIUM)


def test_a_later_reflection_doesnt_move_the_arrival():
    rng = np.random.default_rng(3)
    # A floor bounce nearly as loud, 2.5 ms later: the direct sound still counts, fully trusted.
    ir = fractional_impulse(480, 4096) + 0.9 * fractional_impulse(600, 4096)
    a = alignment.estimate_arrival(capture.analyze_sweep_capture("P", [record(ir, rng)], CFG))
    assert a.ms == pytest.approx(10.0, abs=0.01) and a.confidence == alignment.HIGH, a.reasons
    # A later arrival 3.1 dB louder than the direct sound: still the direct sound, with a caution.
    ir = 0.7 * fractional_impulse(480, 4096) + fractional_impulse(624, 4096)
    a = alignment.estimate_arrival(capture.analyze_sweep_capture("P", [record(ir, rng)], CFG))
    assert a.ms == pytest.approx(10.0, abs=0.01)
    assert a.confidence == alignment.MEDIUM and "stronger than the direct sound" in a.reasons[0]


def test_a_direct_sound_well_under_a_later_arrival_is_missed_but_flagged():
    rng = np.random.default_rng(4)
    ir = 0.35 * fractional_impulse(480, 4096) + fractional_impulse(624, 4096)      # 9 dB down, 3 ms earlier
    a = alignment.estimate_arrival(capture.analyze_sweep_capture("P", [record(ir, rng)], CFG))
    assert a.ms == pytest.approx(13.0, abs=0.01)
    assert a.confidence == alignment.MEDIUM and "earlier arrival 9.1 dB down, 3.0 ms before" in a.reasons[0]


def test_repeats_that_disagree_lower_the_confidence():
    rng = np.random.default_rng(5)
    moved = [record(fractional_impulse(480, 4096), rng), record(fractional_impulse(500, 4096), rng)]   # 0.42 ms apart
    a = alignment.estimate_arrival(capture.analyze_sweep_capture("P", moved, CFG))
    assert a.confidence == alignment.LOW and "disagree" in a.reasons[0]


def test_noise_and_music_captures_are_at_most_medium(sim):
    rng = np.random.default_rng(6)
    x = roomsim.synthetic_program(12.0, FS, rng)
    c = capture.analyze_program_capture("W", x, sim.play(x, 0, roomsim.NoiseSpec(), rng), FS)
    a = alignment.estimate_arrival(c)
    assert a.ms == c.delays_ms[0] and a.confidence == alignment.MEDIUM


def test_a_noisy_capture_is_low():
    rng = np.random.default_rng(7)
    c = capture.analyze_sweep_capture("P", [record(fractional_impulse(480, 4096), rng, noise=0.05)], CFG)
    a = alignment.estimate_arrival(c)
    assert a.confidence == alignment.LOW and any("SNR" in r for r in a.reasons)


def test_distance_and_the_zone_delay_suggestion():
    assert alignment.equivalent_distance_m(12.5) == pytest.approx(4.2875)
    assert alignment.equivalent_distance_m(18.5, latency_ms=6.0) == pytest.approx(4.2875)
    s = alignment.suggest_zone_delay(23.40, 8.20)                  # the feedback's example
    assert s.delay_ms == pytest.approx(15.20) and not s.note
    s = alignment.suggest_zone_delay(23.40, 8.20, main_zone_delay_ms=2.0)   # the mains are delayed too
    assert s.delay_ms == pytest.approx(17.20)
    s = alignment.suggest_zone_delay(8.0, 10.5)
    assert s.delay_ms == 0.0 and s.difference_ms == pytest.approx(-2.5) and "already arrives" in s.note


# ---------------------------------------------------------------------------
# Subs

import scipy.signal as ss   # noqa: E402


def lr4(kind: str, fc: float = 80.0) -> np.ndarray:
    """A Linkwitz-Riley 4th-order crossover half on the alignment grid (two 2nd-order Butterworths)."""
    b = ss.butter(2, fc, btype=kind, fs=FS, output="sos")
    _, h = ss.sosfreqz(np.vstack([b, b]), worN=alignment.LF_GRID, fs=FS)
    return h


def at(h: np.ndarray, extra_ms: float = 0.0, ref_ms: float = 10.0) -> alignment.LowResponse:
    return alignment.LowResponse(h * np.exp(-2j * np.pi * alignment.LF_GRID * extra_ms * 1e-3), ref_ms)


def test_an_lr4_crossover_lines_up_exactly():
    # The mains 3 ms late (their DSP): the sub waits 3 ms, polarity normal, and they sum perfectly.
    out = alignment.align_sub(at(lr4("highpass"), 3.0), at(lr4("lowpass")))
    assert out.ok and out.delay_ms == pytest.approx(3.0, abs=0.011) and not out.invert
    assert out.efficiency_db > -0.05 and out.improvement_db > 1.0
    assert out.region_hz[0] < 80.0 < out.region_hz[1] and out.crossing_hz == pytest.approx(80.0, rel=0.03)
    assert out.confidence == alignment.HIGH, out.reasons
    # Already there: nothing to gain.
    assert alignment.align_sub(at(lr4("highpass"), 3.0), at(lr4("lowpass")), sub_delay_ms=3.0).improvement_db < 0.01


def test_a_reversed_sub_is_inverted_and_a_late_sub_delays_the_mains():
    out = alignment.align_sub(at(lr4("highpass"), 3.0), at(-lr4("lowpass")))
    assert out.invert and out.delay_ms == pytest.approx(3.0, abs=0.011)
    late = alignment.align_sub(at(lr4("highpass")), at(lr4("lowpass"), 2.0))
    assert late.delay_ms == pytest.approx(-2.0, abs=0.011) and "delay the mains by 2.00 ms" in late.note
    # The mains' own zone delay and polarity count: delayed 1 ms and inverted, the sub follows.
    moved = alignment.align_sub(at(lr4("highpass"), 3.0), at(lr4("lowpass")), main_delay_ms=1.0, main_invert=True)
    assert moved.delay_ms == pytest.approx(4.0, abs=0.011) and moved.invert


def test_no_crossover_says_so():
    tops = at(lr4("highpass", 500.0))          # tops that stop at 500 Hz: the sub is louder up to 300 Hz
    out = alignment.align_sub(tops, at(lr4("lowpass", 700.0)))
    assert not out.ok and "No crossover" in out.note


def test_noise_and_a_narrow_overlap_lower_the_confidence():
    out = alignment.align_sub(at(lr4("highpass"), 3.0), at(lr4("lowpass")), main_snr_db=15.0)
    assert out.confidence == alignment.MEDIUM and "15 dB SNR" in out.reasons[0]
    assert alignment.align_sub(at(lr4("highpass"), 3.0), at(lr4("lowpass")), sub_snr_db=5.0).confidence == alignment.LOW


def test_in_the_room_the_prediction_matches_the_sum(sim):
    """Mains (high-passed at 80 Hz, 4 ms more latency) and a sub (low-passed at
    80 Hz) in the simulated room, measured separately at the same spot. Played
    together with the suggestion, the measured sum matches the prediction."""
    rng = np.random.default_rng(81)
    tops = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=80.0, hp_order=4, latency_ms=10.0))
    sub = roomsim.SimulatedRoom(pa=roomsim.PASpec(hp_hz=25.0, hp_order=4, lp_hz=80.0, lp_order=4, peq=(), latency_ms=6.0))
    quiet = roomsim.NoiseSpec(pink_dbfs=-70.0)
    cm = capture.analyze_sweep_capture("M", [tops.play(PLAY, 2, quiet, rng) for _ in range(2)], CFG)
    cs = capture.analyze_sweep_capture("S", [sub.play(PLAY, 2, quiet, rng) for _ in range(2)], CFG)
    out = alignment.align_sub(alignment.low_response(cm), alignment.low_response(cs))
    assert out.ok and 0.0 < out.delay_ms < 12.0, out
    assert out.improvement_db > 1.0

    def together(delay_ms: float, invert: bool) -> alignment.LowResponse:
        shift = int(round(delay_ms * 1e-3 * FS))
        x_sub = np.concatenate([np.zeros(shift), PLAY])[:len(PLAY)] * (-1.0 if invert else 1.0)
        rec = [tops.play(PLAY, 2, quiet, rng) + sub.play(x_sub, 2, roomsim.NoiseSpec(pink_dbfs=-200.0), rng) for _ in range(2)]
        return alignment.low_response(capture.analyze_sweep_capture("MS", rec, CFG))

    lo, hi = out.region_hz
    region = (alignment.LF_GRID >= lo) & (alignment.LF_GRID <= hi)

    def level(r: alignment.LowResponse) -> float:
        return float(np.mean(20 * np.log10(np.abs(r.h[region]))))

    aligned = level(together(round(out.delay_ms * 1e-3 * FS) / FS * 1e3, out.invert))
    as_is = level(together(0.0, False))
    assert aligned == pytest.approx(out.summed_db, abs=0.5)                 # the prediction is what the room does
    assert aligned - as_is == pytest.approx(out.improvement_db, abs=0.5)
