"""tapewatch generate -- build a synthetic tape and its ground truth.

    python -m generate.main --config configs/small.yaml --out results/small

Writes three files:

    tape.csv        the normalised event stream the engine reads
    truth.json      episodes, the participant roster, and generation stats
    generate.json   a manifest: config, seed, counts, defect tallies

The roster in truth.json is the only record of which participants are
market makers. The engine never reads it. Everything the evaluation harness
says about market-maker false positives depends on that separation holding,
so it is worth saying twice.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import time
from typing import Any

from tapewatch import miniyaml, tape
from tapewatch.model import write_json

from .defects import apply_defects
from .simulator import Simulator

MS = 1_000_000
SEC = 1_000_000_000


def build(cfg: dict, out_dir: str) -> dict[str, Any]:
    started = time.time()
    seed = int(cfg.get("seed", 1))
    duration_ns = int(float(cfg["market"]["duration_seconds"]) * SEC)
    symbols = cfg["market"].get("symbols") or ["TWX"]
    if isinstance(symbols, str):
        symbols = [symbols]

    os.makedirs(out_dir, exist_ok=True)

    episode_seq = {"n": 0}

    def next_episode_id() -> int:
        episode_seq["n"] += 1
        return episode_seq["n"]

    all_events: list[tape.Event] = []
    episodes = []
    roster: dict[int, str] = {}
    information_events: list[dict[str, Any]] = []
    sim_stats: dict[str, Any] = {}

    for index, symbol in enumerate(symbols):
        # A per-symbol seed keeps each symbol's tape stable when another
        # symbol is added or removed from the config.
        rng = random.Random(f"{seed}:{symbol}")
        sim = Simulator(symbol, cfg, rng, pid_base=1000 * (index + 1),
                        episode_counter=next_episode_id)
        result = sim.run(duration_ns)
        all_events.extend(result.events)
        episodes.extend(result.episodes)
        roster.update(result.roster)
        information_events.extend(result.information_events)
        sim_stats[symbol] = result.stats
        sim_stats[symbol]["events"] = len(result.events)
        sim_stats[symbol]["episodes"] = len(result.episodes)

    # One sequenced stream. The tie-break on symbol then arrival index is
    # arbitrary but fixed, so the merged tape is reproducible.
    order = {s: i for i, s in enumerate(symbols)}
    all_events.sort(key=lambda e: (e.ts, order.get(e.symbol, 0)))

    seq = 1
    for ev in all_events:
        ev.seq = seq
        ev.eid = seq
        seq += 1

    defect_cfg = cfg.get("feed_defects") or {}
    defect_rng = random.Random(f"{seed}:defects")
    delivered, defect_stats = apply_defects(all_events, defect_cfg, defect_rng)

    tape_path = os.path.join(out_dir, "tape.csv")
    with open(tape_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Tapewatch normalised tape. See include/tapewatch/event.hpp.\n")
        for ev in delivered:
            fh.write(ev.line())
            fh.write("\n")

    by_kind: dict[str, int] = {}
    for ep in episodes:
        by_kind[ep.kind] = by_kind.get(ep.kind, 0) + 1

    truth = {
        "symbols": symbols,
        "seed": seed,
        "duration_seconds": float(cfg["market"]["duration_seconds"]),
        "tick_size": int(cfg["market"].get("tick_size", 1)),
        "roster": {str(k): v for k, v in sorted(roster.items())},
        "market_makers": sorted(p for p, r in roster.items() if r == "market_maker"),
        "manipulators": sorted(
            p for p, r in roster.items()
            if r in ("spoofer", "layerer", "wash_trader", "igniter", "wash_counterparty")
        ),
        "information_events": information_events,
        "episodes": [ep.to_json() for ep in episodes],
    }
    write_json(os.path.join(out_dir, "truth.json"), truth)

    first_ts = delivered[0].ts if delivered else 0
    last_ts = delivered[-1].ts if delivered else 0
    manifest = {
        "config": cfg,
        "seed": seed,
        "symbols": symbols,
        "events_generated": len(all_events),
        "events_delivered": len(delivered),
        "episodes": len(episodes),
        "episodes_by_kind": by_kind,
        "participants": len(roster),
        "roster_counts": _count_roles(roster),
        "first_ts": first_ts,
        "last_ts": last_ts,
        "tape_seconds": (last_ts - first_ts) / SEC if delivered else 0.0,
        "defects": defect_stats,
        "simulation": sim_stats,
        "wall_seconds": round(time.time() - started, 3),
        "tape_path": tape_path,
    }
    write_json(os.path.join(out_dir, "generate.json"), manifest)
    return manifest


def _count_roles(roster: dict[int, str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for role in roster.values():
        out[role] = out.get(role, 0) + 1
    return dict(sorted(out.items()))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Generate a synthetic tape with ground truth.")
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=None, help="override the config seed")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    cfg = miniyaml.load(args.config)
    if args.seed is not None:
        cfg["seed"] = args.seed

    manifest = build(cfg, args.out)
    if not args.quiet:
        print(
            f"generate: {manifest['events_delivered']} events over "
            f"{manifest['tape_seconds']:.1f}s of tape, "
            f"{manifest['episodes']} episodes "
            f"({json.dumps(manifest['episodes_by_kind'], sort_keys=True)}), "
            f"{manifest['wall_seconds']:.1f}s wall"
        )
        d = manifest["defects"]
        print(
            f"generate: defects -- dropped {d['dropped']}, duplicated {d['duplicated']}, "
            f"transposed {d['transposed']}, skewed {d['skewed']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
