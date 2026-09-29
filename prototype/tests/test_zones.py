"""Zones: a sub measured, graded and corrected against its own reference band.

Full-range speakers (mains, fills, delays) are judged against 250 Hz-4 kHz; a
sub has no output there, so it uses 40-100 Hz for grading, level alignment,
the usable range and placing the target.
"""
import numpy as np
import pytest
import scipy.signal as ss

from roomeq import averaging, capture, correction, filters, grading, roomsim, spectrum, sweep, targets

FS = 48000
SUB_BAND = (40.0, 100.0)
SWEEP_CFG = sweep.SweepConfig(duration=5.0)
PLAY = sweep.build_playback(SWEEP_CFG)
NOISE = roomsim.NoiseSpec(pink_dbfs=-60.0)


@pytest.fixture(scope="module")
def sub_room():
    # A sub: 30 Hz high-pass, 100 Hz low-pass (as its processor would), a 55 Hz resonance.
    pa = roomsim.PASpec(hp_hz=30.0, hp_order=4, lp_hz=100.0, lp_order=4, peq=((55.0, 5.0, 2.0),),
                        h2_db=-50.0, h3_db=-55.0)
    return roomsim.SimulatedRoom(pa=pa)


def measure(room, band, rng, prefilter=None):
    play = PLAY if prefilter is None else ss.sosfilt(prefilter, PLAY)
    cfg = capture.AnalysisConfig(grading=grading.GradingConfig(passband=band))
    return [capture.analyze_sweep_capture(f"S{p + 1}", [room.play(play, p, NOISE, rng) for _ in range(2)], SWEEP_CFG, cfg)
            for p in range(4)]


def test_mains_band_is_the_default():
    assert grading.GradingConfig().passband == (250.0, 4000.0)
    assert correction.CorrectionConfig().ref_band == (250.0, 4000.0)
    grid = spectrum.log_freq_grid(20, 20000, 24)
    level = np.where((grid > 60) & (grid < 15000), 0.0, -30.0)
    # The walk starts in the middle of the band: 1 kHz for the mains, as before.
    assert averaging.usable_range(grid, level) == averaging.usable_range(grid, level, start_hz=1000.0)


def test_a_sub_is_graded_measured_and_corrected_on_its_own_band(sub_room):
    caps = measure(sub_room, SUB_BAND, np.random.default_rng(31))
    assert all(c.grade.overall != grading.Grade.REDO for c in caps)
    in_range = [b.name for b in caps[0].grade.bands if not b.out_of_range]
    assert "63" in in_range and "1k" not in in_range            # graded where the sub plays

    summary = averaging.summarize_session(caps, band=SUB_BAND)
    lo, hi = summary.usable
    assert 20.0 <= lo < 40.0 and 90.0 < hi < 170.0

    cfg = correction.CorrectionConfig(ref_band=SUB_BAND, range_hz=(20.0, 150.0))
    res = correction.design_correction(caps, summary, targets.FLAT, FS, cfg)
    assert res.fit_range[1] <= 150.0 and res.bands
    assert all(b.freq <= 150.0 for b in res.bands)
    at_55 = filters.response_db(res.bands, np.array([55.0]), FS)[0]
    assert at_55 < -2.0                                          # the resonance is cut

    # Re-measured through the correction, the sub is flatter across its fit range.
    after = averaging.summarize_session(measure(sub_room, SUB_BAND, np.random.default_rng(32), filters.sos(res.bands, FS)),
                                        band=SUB_BAND)
    judge = (res.grid >= res.fit_range[0]) & (res.grid <= res.fit_range[1]) & ~res.null_mask

    def spread(s):
        level = spectrum.to_db(spectrum.smooth_power(s.freqs, s.power, 3, res.grid, s.weight))[judge]
        return np.sqrt(np.mean((level - np.mean(level)) ** 2))

    assert spread(after) < 0.7 * spread(summary)
