"""Feed damage, injected on purpose.

A clean synthetic tape measures a detector against a feed nobody has. Real
market data arrives with holes in it, and a surveillance system's behaviour
on a damaged feed is not a footnote -- it is most of the operational risk.
The four defects modelled here are the four that change what a detector
concludes:

  drop        an event never arrives. The sequence number jumps, so the gap
              is detectable, but the book is now wrong and stays wrong.
  duplicate   a line appears twice. Left unhandled it double-counts volume,
              which is the input wash-trade scoring is most sensitive to.
  transpose   two adjacent lines arrive in the wrong order while keeping
              their own timestamps. A reorder buffer can fix this; applying
              a cancel before its order cannot be fixed afterwards.
  skew        a timestamp is nudged. When the nudge puts timestamp order and
              sequence order in conflict, neither can be trusted and the
              engine says so instead of picking one.

Rates are per event and configured; the defaults in configs/small.yaml are
small but non-zero, so every run exercises the recovery paths rather than
leaving them to a test nobody runs.
"""

from __future__ import annotations

import random
from typing import Any

from tapewatch import tape

MS = 1_000_000


def apply_defects(events: list[tape.Event], cfg: dict, rng: random.Random
                  ) -> tuple[list[tape.Event], dict[str, Any]]:
    drop_rate = float(cfg.get("drop_rate", 0.0))
    dup_rate = float(cfg.get("duplicate_rate", 0.0))
    transpose_rate = float(cfg.get("transpose_rate", 0.0))
    skew_rate = float(cfg.get("skew_rate", 0.0))
    skew_ms = float(cfg.get("skew_ms", 30.0))

    dropped = duplicated = transposed = skewed = 0
    out: list[tape.Event] = []

    for ev in events:
        if drop_rate > 0 and rng.random() < drop_rate:
            dropped += 1
            continue
        if skew_rate > 0 and rng.random() < skew_rate:
            delta = int(rng.uniform(-skew_ms, skew_ms) * MS)
            ev.ts = max(0, ev.ts + delta)
            skewed += 1
        out.append(ev)
        if dup_rate > 0 and rng.random() < dup_rate:
            # A byte-identical repeat, event id and all. A duplicate with a
            # fresh id is a different bug and a much easier one.
            out.append(tape.Event(**vars(ev)))
            duplicated += 1

    if transpose_rate > 0:
        i = 0
        while i + 1 < len(out):
            if rng.random() < transpose_rate:
                out[i], out[i + 1] = out[i + 1], out[i]
                transposed += 1
                i += 2
            else:
                i += 1

    return out, {
        "dropped": dropped,
        "duplicated": duplicated,
        "transposed": transposed,
        "skewed": skewed,
        "rates": {
            "drop_rate": drop_rate,
            "duplicate_rate": dup_rate,
            "transpose_rate": transpose_rate,
            "skew_rate": skew_rate,
            "skew_ms": skew_ms,
        },
    }
