import numpy as np
import pytest
import scipy.signal as ss

from roomeq import filters, livedelta
from roomeq.filters import BELL, HIGH_SHELF, LOW_SHELF, Band

FS = 48000
ROOM = [Band(LOW_SHELF, 120, 5.0), Band(BELL, 1000, -6.0, 2.0), Band(HIGH_SHELF, 6000, -4.0)]
DELAY_MS = 37.0


def pinkish(seconds: float, seed: int) -> np.ndarray:
    """Pink-ish noise, loud and quiet in turns like music."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    white = rng.normal(0.0, 1.0, n)
    b, a = [0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400]
    x = ss.lfilter(b, a, white)
    envelope = 0.6 + 0.4 * np.sin(2 * np.pi * 0.7 * np.arange(n) / FS)
    return 0.05 * x / np.std(x) * envelope


def through_room(x: np.ndarray, delay_ms: float = DELAY_MS, bands=ROOM, noise_db: float = -40.0, seed: int = 9) -> np.ndarray:
    y = ss.sosfilt(filters.sos(bands, FS), x)
    d = int(round(delay_ms * 1e-3 * FS))
    y = np.concatenate([np.zeros(d), y[:len(y) - d]])
    rng = np.random.default_rng(seed)
    return y + rng.normal(0.0, np.std(y) * 10 ** (noise_db / 20), len(y))


def run(x, y, block=4800, **kw):
    t = livedelta.LiveDeltaTracker(FS, **kw)
    for i in range(0, len(x), block):
        t.push(x[i:i + block], y[i:i + block])
    return t


def expected(grid, bands=ROOM, band=(250.0, 4000.0)):
    db = filters.response_db(bands, grid, FS)
    sel = (grid >= band[0]) & (grid <= band[1])
    return db - np.mean(db[sel])


@pytest.fixture(scope="module")
def tracked():
    x = pinkish(30.0, 1)
    return run(x, through_room(x))


def test_finds_the_delay_and_the_room(tracked):
    r = tracked.result()
    assert r.delay_ms == pytest.approx(DELAY_MS, abs=0.05)
    assert r.frames > 30
    ok = r.valid & (r.grid >= 40) & (r.grid <= 16000)
    assert np.mean(ok[(r.grid >= 40) & (r.grid <= 16000)]) > 0.95
    err = r.delta_db[ok] - expected(r.grid)[ok]
    assert np.max(np.abs(err)) < 0.75, np.max(np.abs(err))
    assert np.sqrt(np.mean(err ** 2)) < 0.25
    band = r.valid & (r.grid >= 250) & (r.grid <= 4000)
    assert np.mean(r.delta_db[band]) == pytest.approx(0.0, abs=1e-9)


def test_any_block_size_gives_the_same_result():
    x = pinkish(14.0, 2)
    y = through_room(x)
    a, b = run(x, y, block=4800).result(), run(x, y, block=1237).result()
    assert a.delay_ms == b.delay_ms and a.frames == b.frames
    np.testing.assert_allclose(a.delta_db, b.delta_db, atol=1e-9, equal_nan=True)
    np.testing.assert_array_equal(a.valid, b.valid)


def test_nothing_until_there_is_music():
    t = run(np.zeros(8 * FS), np.random.default_rng(3).normal(0, 1e-3, 8 * FS))
    r = t.result()
    assert r.delay_ms is None and r.frames == 0 and not np.any(r.valid)
    x = pinkish(12.0, 4)
    for i in range(0, len(x), 4800):
        t.push(x[i:i + 4800], through_room(x)[i:i + 4800])
    assert t.result().delay_ms == pytest.approx(DELAY_MS, abs=0.05)


def test_pauses_leave_the_average():
    x = pinkish(16.0, 5)
    y = through_room(x)
    t = run(x, y)
    noise = np.random.default_rng(6).normal(0, 1e-3, 8 * FS)
    t.push(np.zeros(2 * FS), noise[:2 * FS])   # frames still reaching back into the music count
    before = t.result()
    t.push(np.zeros(6 * FS), noise[2 * FS:])   # silence doesn't
    after = t.result()
    assert after.frames == before.frames
    np.testing.assert_allclose(after.delta_db, before.delta_db, equal_nan=True)


def test_sound_the_console_doesnt_send_is_unreliable():
    # The stage: loud noise at the mic between 200 and 400 Hz that isn't in the console's signal.
    x = pinkish(30.0, 7)
    y = through_room(x)
    sos = ss.butter(4, [200, 400], btype="bandpass", fs=FS, output="sos")
    stage = ss.sosfilt(sos, np.random.default_rng(8).normal(0, 1, len(y)))
    y = y + stage * 4 * np.std(y) / np.std(stage)
    r = run(x, y).result()
    inside = (r.grid >= 240) & (r.grid <= 340)
    assert not np.any(r.valid[inside])
    outside = (r.grid >= 1500) & (r.grid <= 8000)
    assert np.all(r.valid[outside])


def test_a_new_delay_starts_again():
    x = pinkish(40.0, 10)
    y = np.concatenate([through_room(x[:20 * FS]), through_room(x[20 * FS:], delay_ms=52.0)])
    t = livedelta.LiveDeltaTracker(FS)
    for i in range(0, 20 * FS, 4800):
        t.push(x[i:i + 4800], y[i:i + 4800])
    assert t.result().delay_ms == pytest.approx(DELAY_MS, abs=0.05)
    for i in range(20 * FS, len(x), 4800):
        t.push(x[i:i + 4800], y[i:i + 4800])
    r = t.result()
    assert r.delay_ms == pytest.approx(52.0, abs=0.05)
    assert 0 < r.frames <= 31                # averaged since the change was found (the search at 25 s)
    ok = r.valid & (r.grid >= 100) & (r.grid <= 10000)
    assert np.max(np.abs(r.delta_db[ok] - expected(r.grid)[ok])) < 1.0


def test_the_reference_band_sets_0_db():
    x = pinkish(20.0, 11)
    r = run(x, through_room(x), ref_band=(40.0, 100.0)).result()
    sel = r.valid & (r.grid >= 40) & (r.grid <= 100)
    assert np.mean(r.delta_db[sel]) == pytest.approx(0.0, abs=1e-9)
    ok = r.valid & (r.grid >= 40) & (r.grid <= 16000)
    assert np.max(np.abs(r.delta_db[ok] - expected(r.grid, band=(40.0, 100.0))[ok])) < 0.75
