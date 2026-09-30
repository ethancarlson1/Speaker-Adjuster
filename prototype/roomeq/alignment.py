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

from .capture import Capture
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
