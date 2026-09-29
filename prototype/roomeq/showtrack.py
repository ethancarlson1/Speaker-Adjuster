"""Show mode: how the room has changed since soundcheck (Phase 4, delta tracking).

At the end of soundcheck the user stores a reference: the output-to-mic
response per third octave, from ~30 s of music or pink noise
(loudness.transfer_bands_db, so bands the mic didn't hear clearly are NaN).
During the show the same response is measured from the music in 10 s blocks.
Per band, the last 2 minutes of blocks where the band was heard clearly are
averaged and compared with the reference:

- The median change over the bands is a level change after the plugin (an
  amp or a fader), reported on its own; what's left is the tonal change.
- A band whose tonal change reaches 3 dB is flagged, and stays flagged until
  it falls below 2 dB, so the warning doesn't flicker around the threshold.
- A band needs to have been heard clearly in half the window's blocks
  (a minute of music) before it counts at all.

Crowd noise doesn't need special care: it isn't coherent with the output, so
it lowers coherence (fewer blocks count) rather than biasing the level.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

import numpy as np

from .loudness import RECHECK_BANDS

# The bands' usual names (the centres are exact third octaves from 63 Hz).
BAND_NAMES = ("63", "80", "100", "125", "160", "200", "250", "315", "400", "500", "630", "800",
              "1k", "1.25k", "1.6k", "2k", "2.5k", "3.15k", "4k", "5k", "6.3k", "8k")


@dataclass(frozen=True)
class ShowConfig:
    block_s: float = 10.0          # each tracking measurement
    window_s: float = 120.0        # a change has to hold over this much music
    threshold_db: float = 3.0      # flagged at this...
    clear_db: float = 2.0          # ...and cleared below this
    min_fraction: float = 0.5      # of the window's blocks a band must be heard clearly in
    min_bands: int = 6             # bands needed for the level change
    reference_min_bands: int = 10  # bands a reference must have heard clearly


@dataclass
class ShowState:
    delta_db: np.ndarray           # tonal change per band (level change removed), NaN = not enough music yet
    level_db: float                # level change after the plugin, NaN = not enough bands
    flags: list[int]               # per band: +1 / -1 while flagged, else 0
    level_flag: int
    blocks: int                    # blocks in the window
    severity: int                  # 0 none, 1 a change of 3 dB or more, 2 one of 6 dB or more
    message: str = ""
    details: list[str] = field(default_factory=list)


def _range_name(lo: int, hi: int) -> str:
    a, b = BAND_NAMES[lo], BAND_NAMES[hi]
    if lo == hi:
        return f"{a} Hz" if not a.endswith("k") else f"{a[:-1]} kHz"
    if a.endswith("k") and b.endswith("k"):
        return f"{a[:-1]}\u2013{b[:-1]} kHz"
    if b.endswith("k"):
        return f"{a} Hz\u2013{b[:-1]} kHz"
    return f"{a}\u2013{b} Hz"


def _db(v: float) -> str:
    text = f"{abs(v):.1f}"                               # rounds like C's printf, so the C++ port says the same
    return text if text == "0.0" else ("+" if v > 0 else "-") + text


class DeltaTracker:
    def __init__(self, reference: np.ndarray, cfg: ShowConfig = ShowConfig()):
        self.reference = np.asarray(reference, dtype=float)
        self.cfg = cfg
        self.window: deque[np.ndarray] = deque(maxlen=max(1, int(round(cfg.window_s / cfg.block_s))))
        self.flags = [0] * len(self.reference)
        self.level_flag = 0
        self.state = self._evaluate()

    def add_block(self, bands_db: np.ndarray) -> ShowState:
        self.window.append(np.asarray(bands_db, dtype=float))
        self.state = self._evaluate()
        return self.state

    def _flag(self, current: int, value: float) -> int:
        if not np.isfinite(value):
            return 0                                        # not enough clear music in the window: no claim
        sign = 1 if value > 0 else -1
        if current != 0 and current == sign and abs(value) >= self.cfg.clear_db:
            return current
        return sign if abs(value) >= self.cfg.threshold_db else 0

    def _evaluate(self) -> ShowState:
        cfg = self.cfg
        n = len(self.reference)
        need = cfg.min_fraction * self.window.maxlen
        raw = np.full(n, np.nan)
        if self.window:
            blocks = np.vstack(self.window)
            for b in range(n):
                vals = blocks[:, b][np.isfinite(blocks[:, b])]
                if len(vals) >= need and np.isfinite(self.reference[b]):
                    raw[b] = float(np.mean(vals)) - self.reference[b]
        finite = np.isfinite(raw)
        level = float(np.median(raw[finite])) if np.count_nonzero(finite) >= cfg.min_bands else float("nan")
        delta = raw - level if np.isfinite(level) else np.full(n, np.nan)

        self.flags = [self._flag(f, d) for f, d in zip(self.flags, delta)]
        self.level_flag = self._flag(self.level_flag, level)

        sizes = [abs(d) for d, f in zip(delta, self.flags) if f != 0] + ([abs(level)] if self.level_flag else [])
        severity = 0 if not sizes else (2 if max(sizes) >= 6.0 else 1)

        # Neighbouring flagged bands that moved the same way read as one range.
        parts, details = [], []
        b = 0
        while b < n:
            if self.flags[b] == 0:
                b += 1
                continue
            e = b
            while e + 1 < n and self.flags[e + 1] == self.flags[b]:
                e += 1
            peak = max(delta[b:e + 1], key=abs)
            parts.append(f"{_db(peak)} dB at {_range_name(b, e)}")
            b = e + 1
        for i in range(n):
            if np.isfinite(delta[i]):
                details.append(f"{_range_name(i, i)}: {_db(delta[i])} dB" + ("  (flagged)" if self.flags[i] else ""))
        if self.level_flag:
            louder = "louder" if level > 0 else "quieter"
            parts.append(f"everything {abs(level):.1f} dB {louder} after the plugin")
        message = ("Since soundcheck: " + ", ".join(parts)) if parts else ""
        return ShowState(delta_db=delta, level_db=level, flags=list(self.flags), level_flag=self.level_flag,
                         blocks=len(self.window), severity=severity, message=message, details=details)


def reference_is_usable(bands_db: np.ndarray, cfg: ShowConfig = ShowConfig()) -> bool:
    return int(np.count_nonzero(np.isfinite(bands_db))) >= cfg.reference_min_bands


assert len(BAND_NAMES) == len(RECHECK_BANDS)
