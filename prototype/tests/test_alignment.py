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
