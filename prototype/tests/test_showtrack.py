import numpy as np
import pytest
import scipy.signal as ss

from roomeq import loudness, roomsim, showtrack

FS = 48000
N = len(showtrack.BAND_NAMES)


def blocks_like(ref, rng, change=None, count=1):
    out = []
    for _ in range(count):
        b = ref + rng.normal(0, 0.5, N)
        if change is not None:
            b = b + change
        out.append(b)
    return out


def test_steady_room_stays_quiet_and_a_sustained_change_is_flagged():
    rng = np.random.default_rng(1)
    ref = rng.normal(-20, 3, N)
    t = showtrack.DeltaTracker(ref)
    for b in blocks_like(ref, rng, count=12):
        s = t.add_block(b)
    assert s.severity == 0 and s.message == "" and not any(s.flags)
    assert abs(s.level_db) < 0.3

    bump = np.zeros(N)
    bump[3:6] = 4.0                                     # 125-200 Hz up 4 dB
    for i in range(12):
        s = t.add_block(blocks_like(ref, rng, bump)[0])
        if i < 7:                                       # 8 of 12 blocks: the 2-minute average is still under 3 dB
            assert not any(s.flags), i
    assert s.flags[3:6] == [1, 1, 1] and sum(map(abs, s.flags)) == 3
    assert s.severity == 1
    assert s.message.startswith("Since soundcheck: +") and "at 125–200 Hz" in s.message

    # Back to normal: it clears once the average falls under 2 dB, not before.
    cleared = None
    for i in range(12):
        s = t.add_block(blocks_like(ref, rng)[0])
        if cleared is None and not any(s.flags):
            cleared = i
    assert cleared is not None and 4 <= cleared <= 7


def test_a_level_change_is_not_a_tonal_change():
    rng = np.random.default_rng(2)
    ref = rng.normal(-20, 3, N)
    t = showtrack.DeltaTracker(ref)
    for b in blocks_like(ref, rng, np.full(N, 4.5), 12):
        s = t.add_block(b)
    assert s.level_flag == 1 and not any(s.flags)
    assert s.level_db == pytest.approx(4.5, abs=0.3)
    assert s.message.startswith("Since soundcheck: everything 4.") and s.message.endswith("dB louder after the plugin")
    assert s.severity == 1


def test_big_changes_are_severe_and_unheard_bands_do_not_count():
    rng = np.random.default_rng(3)
    ref = rng.normal(-20, 3, N)
    t = showtrack.DeltaTracker(ref)
    change = np.zeros(N)
    change[18:] = -7.0                                  # 4k-8k down 7 dB
    for b in blocks_like(ref, rng, change, 12):
        b[0:3] = np.nan                                 # 63-100 Hz never heard clearly
        s = t.add_block(b)
    assert np.all(np.isnan(s.delta_db[0:3]))
    assert s.flags[18:] == [-1] * 4 and s.severity == 2
    assert "at 4–8 kHz" in s.message and s.message.count("dB at") == 1


def test_needs_a_usable_reference():
    assert not showtrack.reference_is_usable(np.full(N, np.nan))
    assert showtrack.reference_is_usable(np.zeros(N))


def test_in_the_simulated_club(sim):
    """Soundcheck reference from 30 s of music; then the show: 2 minutes as it
    was (with a crowd), then the low mids build up by 5 dB."""
    rng = np.random.default_rng(4)
    x = roomsim.synthetic_program(30.0 + 5 * 60.0, FS, rng)
    crowd = roomsim.NoiseSpec(pink_dbfs=-60.0, babble_dbfs=-42.0)
    y = sim.play(x, 1, crowd, rng)
    A = 10 ** (5 / 40)                                  # a +5 dB bell at 200 Hz, Q 1 (RBJ)
    w0 = 2 * np.pi * 200 / FS
    alpha = np.sin(w0) / 2
    bell = (np.array([1 + alpha * A, -2 * np.cos(w0), 1 - alpha * A]) / (1 + alpha / A),
            np.array([1 + alpha / A, -2 * np.cos(w0), 1 - alpha / A]) / (1 + alpha / A))
    change_at = int((30 + 120) * FS)
    y_changed = ss.lfilter(bell[0], bell[1], y)
    y = np.concatenate([y[:change_at], y_changed[change_at:]])

    ref = loudness.transfer_bands_db(x[:30 * FS], y[:30 * FS], FS)
    assert showtrack.reference_is_usable(ref)
    t = showtrack.DeltaTracker(ref)
    block = 10 * FS
    states = []
    for s in range(30 * FS, len(x) - block + 1, block):
        states.append(t.add_block(loudness.transfer_bands_db(x[s:s + block], y[s:s + block], FS)))
    before = states[11]                                 # two minutes of the unchanged room
    assert before.severity == 0 and not any(before.flags), before.details
    assert np.nanmax(np.abs(before.delta_db)) < 2.0, before.details    # well inside the 3 dB threshold
    after = states[-1]
    flagged = [showtrack.BAND_NAMES[i] for i, f in enumerate(after.flags) if f]
    assert after.severity >= 1 and set(flagged) <= {"125", "160", "200", "250", "315"} and "200" in flagged, after.details
    assert "dB at" in after.message
