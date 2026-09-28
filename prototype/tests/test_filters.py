import numpy as np
import pytest
import scipy.signal as ss
from scipy.interpolate import PchipInterpolator

from roomeq import filters, targets
from roomeq.filters import Band

FS = 48000
F = np.geomspace(20, 20000, 400)


@pytest.mark.parametrize("band", [
    Band(filters.BELL, 1000, -6, 2.0), Band(filters.BELL, 60, 3, 0.7),
    Band(filters.LOW_SHELF, 100, 4), Band(filters.HIGH_SHELF, 8000, -3),
    Band(filters.HIGH_PASS, 80, q=0.7071), Band(filters.LOW_PASS, 12000, q=0.7071),
])
@pytest.mark.parametrize("fs", [44100, 48000, 96000])
def test_band_response_matches_scipy(band, fs):
    _, h = ss.sosfreqz(filters.biquad(band, fs)[None, :], worN=F, fs=fs)
    assert np.max(np.abs(20 * np.log10(np.abs(h)) - filters.band_db(band, F, fs))) < 1e-6


def test_gains_at_defining_frequencies():
    assert filters.band_db(Band(filters.BELL, 1000, -6, 2), np.array([1000.0]), FS)[0] == pytest.approx(-6)
    assert filters.band_db(Band(filters.LOW_SHELF, 100, 4), np.array([100.0]), FS)[0] == pytest.approx(2)
    assert filters.band_db(Band(filters.LOW_SHELF, 100, 4), np.array([20.0]), FS)[0] == pytest.approx(4, abs=0.2)
    assert filters.band_db(Band(filters.HIGH_PASS, 80, q=0.7071), np.array([80.0]), FS)[0] == pytest.approx(-3, abs=0.02)


def test_bandwidth_q_round_trip_and_disabled_bands():
    assert filters.q_for_bandwidth(1.0) == pytest.approx(np.sqrt(2))
    for octaves in (1 / 3, 2 / 3, 1.0, 3.0):
        assert filters.bandwidth_for_q(filters.q_for_bandwidth(octaves)) == pytest.approx(octaves)
    off = Band(filters.BELL, 1000, -6, 2, enabled=False)
    assert np.all(filters.band_db(off, F, FS) == 0)
    assert np.allclose(filters.sos([off], FS), [[1, 0, 0, 1, 0, 0]])
    assert filters.response_db([Band(filters.BELL, 1000, -6, 2), Band(filters.BELL, 1000, 6, 2)], F, FS) \
        == pytest.approx(np.zeros(len(F)), abs=1e-9)


def test_pchip_matches_scipy_and_holds_ends():
    for t in (targets.HOUSE, targets.SPEECH):
        x = np.log2([p[0] for p in t.points])
        y = [p[1] for p in t.points]
        assert np.max(np.abs(PchipInterpolator(x, y)(np.log2(F)) - t.db(F))) < 1e-9
        assert t.db(np.array([10.0, 25000.0])) == pytest.approx([t.points[0][1], t.points[-1][1]])
    assert np.all(targets.FLAT.db(F) == 0)


def test_preset_shapes_match_the_agreed_numbers():
    def at(t, f):
        return float(t.db(np.array([f]))[0])
    # House: +4 dB low lift easing to 0 by 250 Hz, flat mids, -3 dB at 16 kHz.
    assert at(targets.HOUSE, 40) == pytest.approx(4.0)
    assert at(targets.HOUSE, 250) == at(targets.HOUSE, 1000) == at(targets.HOUSE, 2000) == 0.0
    assert at(targets.HOUSE, 16000) == pytest.approx(-3.0)
    assert np.all(np.diff(targets.HOUSE.db(np.geomspace(50, 250, 50))) <= 1e-12)     # monotone, no bumps
    # Speech: -6 dB at 75 Hz, +2 dB presence at 2-4 kHz, -3 dB at 16 kHz.
    assert at(targets.SPEECH, 75) == pytest.approx(-6.0)
    assert at(targets.SPEECH, 2800) == pytest.approx(2.0)
    assert min(at(targets.SPEECH, f) for f in (2000, 3000, 4000)) >= 1.5
    assert at(targets.SPEECH, 16000) == pytest.approx(-3.0)


def test_anchor_offset_places_target_on_the_midband():
    grid = np.geomspace(20, 20000, 300)
    level = -20 + targets.HOUSE.db(grid)
    assert targets.anchor_offset_db(grid, level, targets.HOUSE) == pytest.approx(-20)
