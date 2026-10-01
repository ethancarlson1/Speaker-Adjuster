import numpy as np
import pytest

from roomeq import correction, filters, refit
from roomeq.correction import CorrectionConfig
from roomeq.filters import BELL, HIGH_SHELF, LOW_SHELF, Band

FS = 48000
GRID = refit.refit_grid()
CFG = CorrectionConfig()
X32_OCTAVES = filters.bandwidth_for_q(0.3)

# A nine-band correction: a low shelf, a narrow deep cut and broader ones, boosts, a high shelf.
NINE = [Band(LOW_SHELF, 60, 2.5), Band(BELL, 95, -6, 4.0), Band(BELL, 160, -4, 3.0), Band(BELL, 240, -3, 2.5),
        Band(BELL, 500, 1.5, 1.2), Band(BELL, 1200, -2.5, 2.0), Band(BELL, 2500, 2, 1.3), Band(BELL, 5000, -3, 2.0),
        Band(HIGH_SHELF, 9000, -2)]
CURVE = filters.response_db(NINE, GRID, FS)


@pytest.fixture(scope="module")
def fits():
    return {(n, shelves): refit.refit_bands(NINE, FS, n, shelves=shelves,
                                            max_octaves=CFG.max_octaves if shelves else X32_OCTAVES)
            for n, shelves in [(8, True), (6, True), (4, True), (6, False)]}


def test_bands_that_already_fit_are_kept():
    bands = NINE[:5]
    r = refit.refit_bands(bands, FS, 6)
    assert not r.refitted and r.bands == bands
    assert r.original_bands == 5 and r.max_error_db == 0.0 and r.rms_error_db == 0.0


def test_a_shelf_is_refitted_when_shelves_are_not_allowed():
    bands = [Band(LOW_SHELF, 80, 2.0), Band(BELL, 300, -3, 2.0)]
    r = refit.refit_bands(bands, FS, 6, shelves=False, max_octaves=X32_OCTAVES)
    assert r.refitted and r.bands
    assert all(b.kind == BELL for b in r.bands)
    assert r.max_error_db < 1.0


@pytest.mark.parametrize("n, shelves, max_err, rms", [(8, True, 0.6, 0.2), (6, True, 2.0, 0.7), (4, True, 3.5, 1.0),
                                                      (6, False, 2.0, 0.7)])
def test_refits_within_the_band_count(fits, n, shelves, max_err, rms):
    r = fits[(n, shelves)]
    assert r.refitted and r.original_bands == 9
    assert 0 < len(r.bands) <= n
    resp = filters.response_db(r.bands, GRID, FS)
    got_max, got_rms = refit.fit_error(CURVE, resp)
    assert got_max == pytest.approx(r.max_error_db) and got_rms == pytest.approx(r.rms_error_db)
    assert r.max_error_db < max_err and r.rms_error_db < rms
    # Never boosts beyond what the curve does (the limit is a penalty: a little leeway).
    assert np.max(resp - np.maximum(CURVE, 0.0)) <= refit.BOOST_SLACK_DB + 0.1
    if not shelves:
        assert all(b.kind == BELL for b in r.bands)
    widest = CFG.max_octaves if shelves else X32_OCTAVES
    for b in r.bands:
        assert abs(b.gain_db) <= refit.GAIN_LIMIT_DB
        if b.kind == BELL:
            octaves = filters.bandwidth_for_q(b.q)
            need = CFG.boost_min_octaves if b.gain_db > 0 else correction._min_cut_octaves(b.freq, CFG)
            assert need - 1e-6 <= octaves <= widest + 1e-6, b


def test_more_bands_fit_closer(fits):
    assert fits[(8, True)].max_error_db < fits[(6, True)].max_error_db <= fits[(4, True)].max_error_db + 1e-9


def test_keeps_the_narrow_deep_cut(fits):
    # The backward fit keeps the -6 dB cut at 95 Hz rather than merging it into a broad one.
    assert any(b.kind == BELL and 85 < b.freq < 105 and b.gain_db < -4.5 for b in fits[(6, True)].bands)


def test_no_bands_leaves_the_whole_curve_as_error():
    r = refit.refit_bands(NINE, FS, 0)
    assert r.bands == []
    assert r.max_error_db == pytest.approx(np.max(np.abs(CURVE)))


def test_fit_error():
    curve = np.array([0.0, 0.05, 2.0, -1.0])
    fitted = np.array([0.0, 0.0, 1.0, -1.5])
    mx, rms = refit.fit_error(curve, fitted)
    assert mx == pytest.approx(1.0)
    assert rms == pytest.approx(np.sqrt((1.0 + 0.25) / 2))   # the first two points are flat in both
