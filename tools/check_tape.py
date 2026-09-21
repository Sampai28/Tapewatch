#!/usr/bin/env python3
"""Reconstruct a tape's book and report anything internally inconsistent.

This is a check on the *generator*, not on the engine. A tape whose trades
reference orders that never existed, or whose book crosses, will make every
feed-integrity counter in a detection run measure the generator instead of
the feed. Run it whenever the generator changes.

    python3 tools/check_tape.py results/small/tape.csv
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

from tapewatch import tape  # noqa: E402


def check(path: str, limit: int = 5) -> int:
    books: dict[str, dict[str, dict[int, int]]] = {}
    orders: dict[int, tuple[str, str, int, int]] = {}
    problems: list[str] = []
    crossed = 0
    unknown_maker = 0
    unknown_cancel = 0
    n = 0

    def sides(symbol: str) -> dict[str, dict[int, int]]:
        return books.setdefault(symbol, {"B": {}, "S": {}})

    def add(symbol: str, side: str, price: int, qty: int) -> None:
        d = sides(symbol)[side]
        d[price] = d.get(price, 0) + qty
        if d[price] <= 0:
            del d[price]

    def best(symbol: str, side: str) -> int | None:
        d = sides(symbol)[side]
        if not d:
            return None
        return max(d) if side == "B" else min(d)

    for ev in tape.read(path):
        n += 1
        if ev.kind == "O":
            orders[ev.order_id] = (ev.symbol, ev.side, ev.price, ev.quantity)
            add(ev.symbol, ev.side, ev.price, ev.quantity)
        elif ev.kind == "X":
            o = orders.pop(ev.order_id, None)
            if o is None:
                unknown_cancel += 1
            else:
                add(o[0], o[1], o[2], -o[3])
        elif ev.kind == "M":
            o = orders.get(ev.order_id)
            if o is None:
                unknown_cancel += 1
            else:
                add(o[0], o[1], o[2], -o[3])
                if ev.quantity > 0:
                    orders[ev.order_id] = (o[0], o[1], ev.price, ev.quantity)
                    add(o[0], o[1], ev.price, ev.quantity)
                else:
                    orders.pop(ev.order_id)
        elif ev.kind == "T":
            o = orders.get(ev.maker_order_id)
            if o is None:
                unknown_maker += 1
            else:
                q = min(ev.quantity, o[3])
                add(o[0], o[1], o[2], -q)
                if o[3] - q <= 0:
                    orders.pop(ev.maker_order_id)
                else:
                    orders[ev.maker_order_id] = (o[0], o[1], o[2], o[3] - q)

        bb, ba = best(ev.symbol, "B"), best(ev.symbol, "S")
        if bb is not None and ba is not None and bb >= ba:
            crossed += 1
            if len(problems) < limit:
                problems.append(f"line {n}: crossed {bb} >= {ba} after {ev.line()}")

    print(f"events            {n}")
    print(f"crossed states    {crossed}")
    print(f"unknown maker     {unknown_maker}")
    print(f"unknown cancel    {unknown_cancel}")
    print(f"resting at end    {len(orders)}")
    for p in problems:
        print("  " + p)
    return 1 if crossed else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("tape")
    ap.add_argument("--limit", type=int, default=5)
    args = ap.parse_args(argv)
    return check(args.tape, args.limit)


if __name__ == "__main__":
    raise SystemExit(main())
