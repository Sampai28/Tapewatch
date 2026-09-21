"""tapewatch evaluate -- score a detection run against its ground truth.

    python -m eval.main --config configs/small.yaml --run results/small

Reads tape.csv's companions from the run directory (truth.json, alerts.jsonl,
detect.json, generate.json) and writes eval.json, which is the single source
for every number in the report and in the README.

Three passes:

  1. score the raw alert set at the engine's emission floor. This is the
     "no tuning at all" baseline and it is reported, because a system that
     only looks good after the threshold has been fitted to the answer key
     should be described that way.
  2. sweep each detector's threshold and pick an operating point inside the
     market-maker false positive budget.
  3. re-score at the chosen operating points, and ablate the weights there.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from typing import Any

from tapewatch import miniyaml
from tapewatch.model import DETECTORS, NS_PER_MS, load_alerts, load_truth, write_json

from .ablation import run_ablation
from .matching import match
from .scoring import score, workload
from .store import build_store
from .sweep import apply_operating_points, grid, run_sweep


def _read_json(path: str) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def evaluate(cfg: dict, run_dir: str) -> dict[str, Any]:
    started = time.time()
    truth = load_truth(os.path.join(run_dir, "truth.json"))
    alerts = load_alerts(os.path.join(run_dir, "alerts.jsonl"))
    detect = _read_json(os.path.join(run_dir, "detect.json"))
    generate = _read_json(os.path.join(run_dir, "generate.json"))

    ev = cfg.get("evaluation", {})
    m = ev.get("match", {})
    pre_ns = int(m.get("pre_tolerance_ms", 2000)) * NS_PER_MS
    post_ns = int(m.get("post_tolerance_ms", 20000)) * NS_PER_MS
    triage = float(ev.get("triage_minutes_per_alert", 4))
    analyst_hours = float(ev.get("analyst_hours_per_day", 7.5))
    mm_budget = float(ev.get("mm_false_positives_per_mm_hour", 6.0))
    sweep_cfg = ev.get("sweep", {})
    thresholds = grid(
        float(sweep_cfg.get("start", 0.25)),
        float(sweep_cfg.get("stop", 0.95)),
        float(sweep_cfg.get("step", 0.025)),
    )

    episodes = truth["episodes"]
    market_makers = set(truth["market_makers"])
    tape_seconds = float(detect.get("tape_span_seconds") or generate.get("tape_seconds") or 0.0)

    # --- pass 1: raw -----------------------------------------------------
    raw_result = match(alerts, episodes, pre_ns, post_ns)
    raw = score(raw_result, tape_seconds, market_makers, DETECTORS)
    raw["workload"] = workload(raw, triage, analyst_hours)

    # --- pass 2: sweep ---------------------------------------------------
    sweep = run_sweep(alerts, episodes, DETECTORS, thresholds, tape_seconds, market_makers,
                      pre_ns, post_ns, mm_budget)

    chosen = {
        name: entry["operating_point"].get("threshold")
        for name, entry in sweep["detectors"].items()
    }

    # --- pass 3: tuned + ablation ---------------------------------------
    tuned_alerts = apply_operating_points(alerts, sweep)
    tuned_result = match(tuned_alerts, episodes, pre_ns, post_ns)
    tuned = score(tuned_result, tape_seconds, market_makers, DETECTORS)
    tuned["workload"] = workload(tuned, triage, analyst_hours)

    ablation = run_ablation(alerts, episodes, DETECTORS,
                            {k: v for k, v in chosen.items() if v is not None},
                            tape_seconds, market_makers, pre_ns, post_ns)

    misses = [
        {
            "episode_id": ep.episode_id,
            "kind": ep.kind,
            "participant": ep.participant,
            "intensity": round(ep.intensity, 3),
            "start_ts": ep.start_ts,
            "duration_ms": (ep.end_ts - ep.start_ts) // NS_PER_MS,
            "offtype_alerts": len(tuned_result.episode_offtype.get(ep.episode_id, [])),
            "notes": ep.notes,
        }
        for ep in tuned_result.missed()
    ]

    roster = {int(k): v for k, v in truth["roster"].items()}
    fp_by_role: dict[str, int] = {}
    for a in tuned_result.false_positives:
        role = roster.get(a.participant, "unknown")
        fp_by_role[role] = fp_by_role.get(role, 0) + 1

    out = {
        "run_dir": run_dir,
        "tape_seconds": tape_seconds,
        "match_rule": {
            "pre_tolerance_ms": pre_ns // NS_PER_MS,
            "post_tolerance_ms": post_ns // NS_PER_MS,
            "typed": "alert.detector must equal episode.kind for a true positive",
        },
        "counts": {
            "alerts_raw": len(alerts),
            "alerts_tuned": len(tuned_alerts),
            "episodes": len(episodes),
            "suppressed_by_cooldown": detect.get("alerts_suppressed_cooldown", 0),
            "degraded_alerts": sum(1 for a in alerts if a.degraded),
        },
        "raw": raw,
        "sweep": sweep,
        "operating_points": chosen,
        "tuned": tuned,
        "ablation": ablation,
        "false_positives_by_role": dict(sorted(fp_by_role.items(), key=lambda kv: -kv[1])),
        "missed_episodes": sorted(misses, key=lambda d: (d["kind"], -d["intensity"])),
        "feed": detect.get("feed", {}),
        "book": detect.get("book", {}),
        "engine": {
            "events": detect.get("events_admitted", 0),
            "wall_seconds": detect.get("wall_seconds", 0.0),
            "events_per_second": detect.get("events_per_second", 0.0),
            "scans": detect.get("scans", 0),
            "scan_interval_ms": detect.get("config", {}).get("scan_interval_ms"),
        },
        "generation": {
            "events": generate.get("events_delivered", 0),
            "wall_seconds": generate.get("wall_seconds", 0.0),
            "defects": generate.get("defects", {}),
            "roster_counts": generate.get("roster_counts", {}),
        },
        "eval_wall_seconds": round(time.time() - started, 3),
    }
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Evaluate alerts against ground truth.")
    ap.add_argument("--config", required=True)
    ap.add_argument("--run", required=True, help="run directory holding tape/alerts/truth")
    ap.add_argument("--out", default=None, help="defaults to <run>/eval.json")
    ap.add_argument("--db", default=None, help="also build the console's SQLite store here")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    cfg = miniyaml.load(args.config)
    result = evaluate(cfg, args.run)
    out_path = args.out or os.path.join(args.run, "eval.json")
    write_json(out_path, result)

    db_path = args.db or os.path.join(args.run, "tapewatch.db")
    build_store(db_path, args.run, result)

    if not args.quiet:
        t = result["tuned"]["overall"]
        mm = result["tuned"]["market_maker"]
        print(
            f"eval: at the chosen operating points -- "
            f"precision {t['precision']:.3f}, recall {t['recall']:.3f}, F1 {t['f1']:.3f}, "
            f"{t['alerts']} alerts, latency p50 {t['latency_ms']['p50']:.0f}ms"
        )
        print(
            f"eval: market makers -- {mm['false_positives']} false positives "
            f"({mm['share_of_false_positives']*100:.0f}% of all FPs), "
            f"{mm['per_mm_per_hour']:.2f} per market maker per hour"
        )
        print(f"eval: wrote {out_path} and {db_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
