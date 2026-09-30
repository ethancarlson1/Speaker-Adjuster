"""Alignment: when each speaker's sound arrives, and what delay lines them up.

Arrival: the direct sound's arrival, from the capture's impulse response:
the first peak within 6 dB of the strongest one, refined below a sample
(so a floor bounce or a wall that's nearly as loud, arriving later, doesn't
move it). Pink noise and music: the dual-FFT's delay. It's a loop delay: it
includes the interface and host latency as well as the flight time, so
arrivals are compared with each other, or have a measured system latency
(a loopback) taken off.

Confidence says how far to trust it:
- repeats: the sweep repeats agree on the arrival;
- the direct sound: a later arrival more than 3 dB stronger, or an earlier
  one only a little under the 6 dB threshold, means the direct sound may be
  partly blocked, so the true arrival is uncertain;
- noise: the bands that set the arrival (the capture's highest graded
  bands) were measured well above the noise.
Pink noise and music captures are at most medium: their delay is only
found to the nearest sample, without an impulse response to check.
"""

from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .capture import LF_GRID, Capture
from .spectrum import WindowConfig, ir_window

SPEED_OF_SOUND = 343.0          # m/s, about 20 °C

HIGH, MEDIUM, LOW = "high", "medium", "low"
FIRST_ARRIVAL_DB = 6.0          # the direct sound: the first peak within this of the strongest
EARLIER_WARNING_DB = 12.0       # an earlier peak within this of the strongest is worth a mention
_RANK = {HIGH: 0, MEDIUM: 1, LOW: 2}


@dataclass
class Arrival:
    ms: float                                  # loop delay of the main arrival
    confidence: str = HIGH
    reasons: list[str] = field(default_factory=list)


def _worse(a: str, b: str) -> str:
    return a if _RANK[a] >= _RANK[b] else b


def estimate_arrival(c: Capture, separation_ms: float = 1.0, window: WindowConfig = WindowConfig()) -> Arrival:
    """The capture's direct arrival and how far to trust it. separation_ms: how
    far before the arrival an earlier peak has to be to count as a separate
    arrival (1 ms for full-range speakers; about a period for subs)."""
    if not c.delays_ms:
        return Arrival(float("nan"), LOW, ["no delay was measured"])
    ms = float(c.delays_ms[0])
    conf, reasons = HIGH, []

    if c.ir is not None and len(c.ir) > 2:
        ir = np.abs(np.asarray(c.ir, dtype=float))
        n_pre = ir_window(window, c.fs)[1]
        strongest = int(np.argmax(ir))
        # The first peak within 6 dB of the strongest: the first sample over the
        # threshold, then up its slope to the top.
        threshold = ir[strongest] * 10 ** (-FIRST_ARRIVAL_DB / 20)
        pk = int(np.argmax(ir >= threshold))
        while pk + 1 < len(ir) and ir[pk + 1] > ir[pk]:
            pk += 1
        # Below a sample: a parabola through the peak and its neighbours.
        if 0 < pk < len(ir) - 1:
            y0, y1, y2 = ir[pk - 1], ir[pk], ir[pk + 1]
            denom = y0 - 2 * y1 + y2
            frac = float(np.clip(0.5 * (y0 - y2) / denom, -0.5, 0.5)) if denom != 0 else 0.0
        else:
            frac = 0.0
        ms += 1000.0 * (pk - n_pre + frac) / c.fs

        # Repeats.
        spread = max(c.delays_ms) - min(c.delays_ms)
        if spread > 0.25:
            conf = _worse(conf, LOW)
            reasons.append(f"the repeat sweeps disagree about the arrival by {spread:.2f} ms (something moved)")
        elif spread > 0.05:
            conf = _worse(conf, MEDIUM)
            reasons.append(f"the repeat sweeps differ by {spread:.2f} ms")

        # The direct sound: weaker than a later arrival, or something earlier nearly counted.
        below_db = 20 * np.log10(ir[strongest] / max(ir[pk], 1e-30))
        if below_db > 3.0:
            conf = _worse(conf, MEDIUM)
            reasons.append(f"a later arrival is {below_db:.1f} dB stronger than the direct sound, "
                           f"{1000.0 * (strongest - pk) / c.fs:.1f} ms after it (the direct sound may be partly blocked)")
        sep = int(round(separation_ms * 1e-3 * c.fs))
        before = ir[:max(pk - sep, 0)]
        if len(before):
            k = int(np.argmax(before))
            down_db = 20 * np.log10(ir[strongest] / max(before[k], 1e-30))
            if down_db < EARLIER_WARNING_DB:
                conf = _worse(conf, MEDIUM)
                reasons.append(f"an earlier arrival {down_db:.1f} dB down, {1000.0 * (pk - k) / c.fs:.1f} ms before: "
                               "if that's the direct sound, it arrives that much earlier")
    else:
        conf = _worse(conf, MEDIUM)
        reasons.append("measured from pink noise or music: to the nearest sample, with no impulse response to check")

    # Noise in the bands that set the arrival: the three highest graded ones.
    graded = [b for b in c.grade.bands if not b.out_of_range]
    top = graded[-3:]
    if top:
        worst = min(b.snr_db for b in top)
        if worst < 10.0:
            conf = _worse(conf, LOW)
            reasons.append(f"noisy: {worst:.0f} dB SNR where the arrival is set")
        elif worst < 20.0:
            conf = _worse(conf, MEDIUM)
            reasons.append(f"{worst:.0f} dB SNR where the arrival is set")
    else:
        conf = _worse(conf, LOW)
        reasons.append("no band was heard clearly")
    return Arrival(ms, conf, reasons)


def equivalent_distance_m(arrival_ms: float, latency_ms: float = 0.0) -> float:
    """How far sound travels in the arrival time, less a measured system latency.
    An equivalent acoustic distance, not necessarily the physical one."""
    return (arrival_ms - latency_ms) * 1e-3 * SPEED_OF_SOUND


@dataclass
class DelaySuggestion:
    delay_ms: float            # add this to the zone's delay (never negative)
    difference_ms: float       # main arrival - this zone's arrival (both measured without their zone delays)
    note: str = ""


def suggest_zone_delay(main_ms: float, zone_ms: float, main_zone_delay_ms: float = 0.0) -> DelaySuggestion:
    """The delay that makes this zone's sound arrive with the main system's,
    at the spot both were measured at. Measurements bypass the zone delays,
    so the main system's own zone delay (if any) is added back."""
    diff = main_ms + main_zone_delay_ms - zone_ms
    if diff >= 0:
        return DelaySuggestion(diff, diff)
    return DelaySuggestion(0.0, diff, f"this zone already arrives {-diff:.2f} ms after the main system here: "
                                      "delay the main system instead, or measure where the two overlap")


# ---------------------------------------------------------------------------
# Subs: aligned by phase over the crossover, not by arrival
#
# A sub's impulse response has a broad peak, so its arrival says little. Each
# sweep keeps its complex response below 1 kHz instead (from the loop start,
# so it holds the direct sound however late the room modes peak), and two of
# them from the same spot predict how the mains and the sub sum for any sub
# delay and polarity. The crossover region is where the two are within 10 dB
# of each other around the frequency where they cross (smoothed to 1/6
# octave). The suggestion is the sub delay and polarity that give the loudest
# sum over that region. When several are within 0.25 dB (half a period apart
# with the polarity flipped, or a period apart), it takes one the sub can
# apply itself (not a negative delay), then normal polarity, then the
# smallest delay.

REGION_DB = 10.0
SEARCH_MS = 20.0                       # sub delays searched: -20..+20 ms (negative: delay the mains instead)
STEP_MS = 0.01
NEAR_DB = 0.25


@dataclass
class LowResponse:
    """A complex response on LF_GRID whose time 0 is ref_ms after the loop start:
    the response as heard is h * exp(-j 2 pi f ref_ms / 1000)."""
    h: np.ndarray
    ref_ms: float


def low_response(c: Capture) -> LowResponse | None:
    """A sweep's (None for pink noise and music)."""
    if c.low is None:
        return None
    return LowResponse(np.asarray(c.low), 0.0)


def _smoothed_db(h: np.ndarray, half_width: int = 2) -> np.ndarray:
    """1/6 octave (+-2 points at 24 per octave) power average, in dB."""
    p = np.abs(h) ** 2
    out = np.empty(len(p))
    for i in range(len(p)):
        lo, hi = max(i - half_width, 0), min(i + half_width + 1, len(p))
        out[i] = np.mean(p[lo:hi])
    return 10 * np.log10(np.maximum(out, 1e-30))


def band_snr(bands, lo: float, hi: float) -> float:
    """The lowest SNR among graded octave bands overlapping lo-hi (inf if none)."""
    snrs = [b.snr_db for b in bands if not b.out_of_range and b.hi > lo and b.lo < hi]
    return min(snrs) if snrs else float("inf")


@dataclass
class SubAlignment:
    ok: bool
    region_hz: tuple[float, float] = (0.0, 0.0)
    crossing_hz: float = 0.0
    delay_ms: float = 0.0            # the sub's zone delay; negative: delay the mains by -delay_ms instead
    invert: bool = False             # the sub's polarity
    summed_db: float = 0.0           # the mean summed level over the region with the suggestion (dB, as measured)
    improvement_db: float = 0.0      # the summed level over the region, against the current settings
    efficiency_db: float = 0.0       # how close to a perfect sum (0 dB) the suggestion gets
    confidence: str = HIGH
    reasons: list[str] = field(default_factory=list)
    note: str = ""


def _crossover(hm: np.ndarray, hs: np.ndarray) -> tuple[int, int, int] | str:
    """(lo, crossing, hi) indices on LF_GRID, or why there's no crossover. Only
    the magnitudes count, so delays and polarity don't move it."""
    f = LF_GRID
    d = _smoothed_db(hm) - _smoothed_db(hs)
    inside = np.flatnonzero((f >= 30.0) & (f <= 300.0))
    crossing = next((int(i) for i in inside[1:] if d[i - 1] < 0.0 <= d[i]), None)
    if crossing is None:
        return ("the sub is louder than the mains all the way up to 300 Hz" if np.all(d[inside] < 0) else
                "the mains are louder than the sub all the way down to 30 Hz" if np.all(d[inside] >= 0) else
                "the mains and the sub never cross over between 30 and 300 Hz")
    lo = hi = crossing
    while lo > inside[0] and abs(d[lo - 1]) <= REGION_DB:
        lo -= 1
    while hi < inside[-1] and abs(d[hi + 1]) <= REGION_DB:
        hi += 1
    return lo, crossing, hi


def crossover_region(main: LowResponse, sub: LowResponse) -> tuple[float, float] | None:
    """The crossover region's edges (Hz), where each measurement's SNR counts
    (band_snr); None when they don't cross."""
    x = _crossover(main.h, sub.h)
    return None if isinstance(x, str) else (float(LF_GRID[x[0]]), float(LF_GRID[x[2]]))


def align_sub(main: LowResponse, sub: LowResponse, main_delay_ms: float = 0.0, main_invert: bool = False,
              sub_delay_ms: float = 0.0, sub_invert: bool = False,
              main_snr_db: float = float("inf"), sub_snr_db: float = float("inf")) -> SubAlignment:
    """The sub delay and polarity that sum best with the mains over the crossover,
    from the two measurements taken at the same spot (without their zone delays).
    main_delay_ms / main_invert: the mains' current zone settings; sub_delay_ms /
    sub_invert: the sub's (the improvement is against them). *_snr_db: the lowest
    SNR of each measurement over the crossover (band_snr over crossover_region)."""
    f = LF_GRID
    w = 2 * np.pi * f * 1e-3
    hm = main.h * np.exp(-1j * w * (main.ref_ms + main_delay_ms)) * (-1.0 if main_invert else 1.0)
    hs = sub.h * np.exp(-1j * w * sub.ref_ms)

    # The crossover: where they cross (sub louder below, mains louder above), and within 10 dB around it.
    x = _crossover(hm, hs)
    if isinstance(x, str):
        return SubAlignment(False, note=f"No crossover to align: {x}. Check both were measured at the same spot.")
    lo, crossing, hi = x
    region = np.arange(lo, hi + 1)
    fr, a, b = f[region], hm[region], hs[region]

    def summed(delay: np.ndarray, sign: float) -> np.ndarray:
        """Mean summed level (dB) over the region for each delay."""
        s = a[None, :] + sign * b[None, :] * np.exp(-2j * np.pi * fr[None, :] * delay[:, None] * 1e-3)
        return np.mean(20 * np.log10(np.maximum(np.abs(s), 1e-30)), axis=1)

    delays = np.round(np.arange(-SEARCH_MS, SEARCH_MS + STEP_MS / 2, STEP_MS), 2)
    candidates = []                      # local maxima: (level, delay, invert)
    for invert in (False, True):
        level = summed(delays, -1.0 if invert else 1.0)
        for i in range(len(delays)):
            left = level[i - 1] if i > 0 else -np.inf
            right = level[i + 1] if i + 1 < len(delays) else -np.inf
            if level[i] >= left and level[i] > right:
                candidates.append((float(level[i]), float(delays[i]), invert))
    best_level = max(c[0] for c in candidates)
    level, delay, invert = min((c for c in candidates if c[0] >= best_level - NEAR_DB),
                               key=lambda c: (c[1] < 0, c[2], abs(c[1])))

    now = float(summed(np.array([sub_delay_ms]), -1.0 if sub_invert else 1.0)[0])
    ideal = float(np.mean(20 * np.log10(np.abs(a) + np.abs(b))))
    out = SubAlignment(True, (float(fr[0]), float(fr[-1])), float(f[crossing]), delay, invert, level, level - now, level - ideal)

    # How far to trust it.
    snr = min(main_snr_db, sub_snr_db)
    if snr < 10.0:
        out.confidence = _worse(out.confidence, LOW)
        out.reasons.append(f"noisy: {snr:.0f} dB SNR over the crossover")
    elif snr < 20.0:
        out.confidence = _worse(out.confidence, MEDIUM)
        out.reasons.append(f"{snr:.0f} dB SNR over the crossover")
    if np.log2(fr[-1] / fr[0]) < 1.0 / 3.0:
        out.confidence = _worse(out.confidence, MEDIUM)
        out.reasons.append("the mains and the sub overlap over less than a third of an octave")
    # A delay a whole period away summing about as well: the overlap doesn't pin the timing down.
    period_ms = 1000.0 / out.crossing_hz
    others = [c for c in candidates if c[2] == invert and abs(c[1] - delay) >= 0.75 * period_ms]
    if others:
        alt = max(others, key=lambda c: c[0])
        margin = level - alt[0]
        if margin < 0.3:
            out.confidence = _worse(out.confidence, MEDIUM)
            out.reasons.append(f"{alt[1]:.2f} ms, a period away, sums about as well ({margin:.1f} dB less)")
    if delay < 0:
        out.note = (f"The sub arrives {-delay:.2f} ms late here: delay the mains by {-delay:.2f} ms instead "
                    "(and anything lined up with them)")
    return out
