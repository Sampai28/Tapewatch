#!/usr/bin/env python3
"""Convert a Matchbook run into a Tapewatch tape.

Matchbook (the sibling limit order book project) writes two files, and
neither one is surveillable on its own:

  the input stream, from tools/flowgen.py --
      N,<client_id>,<participant>,<B|S>,<LIMIT|MARKET|IOC|FOK|POST_ONLY>,<px|->,<qty>
      C,<client_id>,<participant>
      R,<orig_id>,<new_id>,<participant>,<px|->,<qty>
  ...which carries the participant but has no timestamps and no symbol.

  the event log, from matchbook-replay --
      A,<seq>,<client_id>,<status>
      F,<seq>,<taker_id>,<maker_id>,<price>,<qty>,<taker_side>
      X,<seq>,<client_id>
      R,<seq>,<client_id>,<reason>
  ...which carries the sequence number and both sides of every fill, but has
  no participant, no symbol, and -- deliberately, so that replays are
  byte-identical -- no timestamps at all.

Surveillance needs participant, time and sequence together, so this joins the
two on client order id. Three things are reconstructed rather than read, and
all three are approximations:

  participant   taken from the input stream. Exact.
  symbol        taken from --symbol. Matchbook replays one book at a time.
  timestamp     synthesised from sequence position at --rate messages per
                second. This is the significant one: Matchbook's log has no
                time in it, so every duration a detector measures on an
                adapted tape -- order lifetime, cancel clustering, reversal
                windows -- is a function of this flag rather than of what
                actually happened. Detector thresholds tuned on a synthetic
                tape do not transfer to an adapted one without re-tuning.

Tapewatch's own generator exists so that the evaluation never depends on any
of that. This adapter is for replaying real Matchbook flow through the
detectors, not for measuring them.

    python3 tools/matchbook_adapter.py \\
        --stream ../Matchbook/tests/fixtures/recorded_stream.csv \\
        --log /tmp/v0.log --symbol MBK --out results/matchbook/tape.csv
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

from tapewatch import tape  # noqa: E402

NS_PER_SEC = 1_000_000_000


def read_stream(path: str) -> tuple[dict[int, dict], list[str]]:
    """Client order id -> {participant, side, price, qty, type}."""
    orders: dict[int, dict] = {}
    warnings: list[str] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line_no, raw in enumerate(fh, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            f = line.split(",")
            kind = f[0]
            try:
                if kind == "N" and len(f) == 7:
                    orders[int(f[1])] = {
                        "participant": int(f[2]),
                        "side": f[3],
                        "type": f[4],
                        "price": None if f[5] == "-" else int(f[5]),
                        "qty": int(f[6]),
                    }
                elif kind == "C" and len(f) == 3:
                    pass  # the log's X record carries the cancel
                elif kind == "R" and len(f) == 6:
                    original = orders.get(int(f[1]), {})
                    orders[int(f[2])] = {
                        "participant": int(f[3]),
                        "side": original.get("side", "B"),
                        "type": "LIMIT",
                        "price": original.get("price") if f[4] == "-" else int(f[4]),
                        "qty": int(f[5]),
                    }
                else:
                    warnings.append(f"{path}:{line_no}: unrecognised record {line!r}")
            except ValueError:
                warnings.append(f"{path}:{line_no}: malformed record {line!r}")
    return orders, warnings


def convert(stream_path: str, log_path: str, out_path: str, symbol: str, rate: float,
            start_ns: int) -> dict:
    orders, warnings = read_stream(stream_path)
    interval = int(NS_PER_SEC / rate) if rate > 0 else 1
    counts = {"order": 0, "cancel": 0, "trade": 0, "reject": 0, "unknown_client": 0}

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    with open(log_path, "r", encoding="utf-8") as log, \
         open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write(f"# Adapted from Matchbook: stream={stream_path} log={log_path}\n")
        out.write(f"# Timestamps are synthesised at {rate:g} messages/second. "
                  f"Matchbook's log carries none.\n")
        writer = tape.Writer(out)
        index = 0
        for raw in log:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            f = line.split(",")
            ts = start_ns + index * interval
            index += 1
            kind = f[0]

            if kind == "A" and len(f) == 4:
                cid = int(f[2])
                o = orders.get(cid)
                if o is None:
                    counts["unknown_client"] += 1
                    continue
                # Only resting orders become book state. Marketable types
                # never appear as an O record; their fills do.
                if o["type"] in ("MARKET", "IOC", "FOK") or o["price"] is None:
                    continue
                writer.write(tape.Event(tape.ORDER, ts=ts, seq=0, eid=0, symbol=symbol,
                                        participant=o["participant"], order_id=cid,
                                        side=o["side"], price=o["price"], quantity=o["qty"]))
                counts["order"] += 1
            elif kind == "X" and len(f) == 3:
                cid = int(f[2])
                o = orders.get(cid)
                if o is None:
                    counts["unknown_client"] += 1
                    continue
                writer.write(tape.Event(tape.CANCEL, ts=ts, seq=0, eid=0, symbol=symbol,
                                        participant=o["participant"], order_id=cid))
                counts["cancel"] += 1
            elif kind == "F" and len(f) == 7:
                taker, maker = int(f[2]), int(f[3])
                to = orders.get(taker)
                mo = orders.get(maker)
                if to is None or mo is None:
                    counts["unknown_client"] += 1
                    continue
                writer.write(tape.Event(tape.TRADE, ts=ts, seq=0, eid=0, symbol=symbol,
                                        participant=to["participant"], order_id=taker,
                                        maker_participant=mo["participant"],
                                        maker_order_id=maker, side=f[6],
                                        price=int(f[4]), quantity=int(f[5])))
                counts["trade"] += 1
            elif kind == "R":
                counts["reject"] += 1
            else:
                warnings.append(f"{log_path}: unrecognised log record {line!r}")

        counts["written"] = writer.count

    return {"counts": counts, "warnings": warnings[:20], "warning_count": len(warnings)}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stream", required=True, help="Matchbook input stream (flowgen format)")
    ap.add_argument("--log", required=True, help="Matchbook event log (A/F/X/R records)")
    ap.add_argument("--out", required=True, help="Tapewatch tape to write")
    ap.add_argument("--symbol", default="MBK")
    ap.add_argument("--rate", type=float, default=1000.0,
                    help="synthesised messages per second (default 1000)")
    ap.add_argument("--start-seconds", type=float, default=60.0,
                    help="tape start, so detectors have room before the first event")
    args = ap.parse_args(argv)

    result = convert(args.stream, args.log, args.out, args.symbol, args.rate,
                     int(args.start_seconds * NS_PER_SEC))
    c = result["counts"]
    print(f"adapter: wrote {c['written']} events to {args.out} "
          f"({c['order']} orders, {c['cancel']} cancels, {c['trade']} trades, "
          f"{c['reject']} rejects skipped)")
    if c["unknown_client"]:
        print(f"adapter: {c['unknown_client']} log records referenced a client order id "
              f"the input stream does not contain")
    for w in result["warnings"]:
        print(f"adapter: {w}")
    if result["warning_count"] > len(result["warnings"]):
        print(f"adapter: and {result['warning_count'] - len(result['warnings'])} more warnings")
    print("adapter: timestamps are synthesised -- see the note at the top of this file "
          "before reading any duration off the result")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
