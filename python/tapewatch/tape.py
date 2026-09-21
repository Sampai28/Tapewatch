"""Reading and writing the normalised Tapewatch tape.

The format is defined in include/tapewatch/event.hpp and is a strict superset
of what Matchbook's own files carry. Keeping the Python writer and the C++
reader in the same shape is checked by python/tests/test_tape.py, which parses
a tape the engine has already accepted.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator, TextIO

ORDER = "O"
CANCEL = "X"
MODIFY = "M"
TRADE = "T"


@dataclass
class Event:
    kind: str
    ts: int
    seq: int
    eid: int
    symbol: str
    participant: int = 0
    order_id: int = 0
    maker_participant: int = 0
    maker_order_id: int = 0
    side: str = "B"
    price: int = 0
    quantity: int = 0

    def line(self) -> str:
        if self.kind in (ORDER, MODIFY):
            return (
                f"{self.kind},{self.ts},{self.seq},{self.eid},{self.symbol},"
                f"{self.participant},{self.order_id},{self.side},{self.price},{self.quantity}"
            )
        if self.kind == CANCEL:
            return (
                f"X,{self.ts},{self.seq},{self.eid},{self.symbol},"
                f"{self.participant},{self.order_id}"
            )
        if self.kind == TRADE:
            return (
                f"T,{self.ts},{self.seq},{self.eid},{self.symbol},"
                f"{self.participant},{self.order_id},"
                f"{self.maker_participant},{self.maker_order_id},"
                f"{self.side},{self.price},{self.quantity}"
            )
        raise ValueError(f"unknown event kind {self.kind!r}")


def parse(line: str) -> Event | None:
    line = line.strip()
    if not line or line.startswith("#"):
        return None
    f = line.split(",")
    kind = f[0]
    if kind in (ORDER, MODIFY):
        if len(f) != 10:
            raise ValueError(f"expected 10 fields, got {len(f)}: {line!r}")
        return Event(kind, int(f[1]), int(f[2]), int(f[3]), f[4], int(f[5]), int(f[6]),
                     side=f[7], price=int(f[8]), quantity=int(f[9]))
    if kind == CANCEL:
        if len(f) != 7:
            raise ValueError(f"expected 7 fields, got {len(f)}: {line!r}")
        return Event(kind, int(f[1]), int(f[2]), int(f[3]), f[4], int(f[5]), int(f[6]))
    if kind == TRADE:
        if len(f) != 12:
            raise ValueError(f"expected 12 fields, got {len(f)}: {line!r}")
        return Event(kind, int(f[1]), int(f[2]), int(f[3]), f[4], int(f[5]), int(f[6]),
                     maker_participant=int(f[7]), maker_order_id=int(f[8]),
                     side=f[9], price=int(f[10]), quantity=int(f[11]))
    raise ValueError(f"unknown record kind {kind!r}")


def read(path: str) -> Iterator[Event]:
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            ev = parse(line)
            if ev is not None:
                yield ev


class Writer:
    """Assigns sequence numbers and event ids as events are written.

    Both counters live here rather than in the simulation so that the tape is
    contiguous by construction and any gap in the delivered file is one the
    defect injector put there on purpose.
    """

    def __init__(self, fh: TextIO) -> None:
        self.fh = fh
        self.seq = 1
        self.eid = 1
        self.count = 0

    def write(self, ev: Event) -> Event:
        ev.seq = self.seq
        ev.eid = self.eid
        self.seq += 1
        self.eid += 1
        self.count += 1
        self.fh.write(ev.line())
        self.fh.write("\n")
        return ev
