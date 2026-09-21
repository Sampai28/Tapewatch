"""Threshold sweep and operating-point selection.

The engine emits every candidate above a low floor and records the signal
vector behind each one. The operating point is chosen here, afterwards, by
re-scoring those vectors -- so the whole precision-recall curve costs one
detection run, and the curve and the shipped configuration cannot disagree
about what the detector computed.

Choosing the point is not "take the best F1". A surveillance system with the
best F1 in the world is switched off within a month if it buries the desk in
false positives against its own market makers, so the rule here is:

    the highest-F1 threshold whose market-maker false positive rate stays
    inside the configured budget

and if no threshold satisfies the budget, that is reported as a failure to
find an operating point rather than quietly relaxed.
"""

from __future__ import annotations

from typing import Any, Iterable

from tapewatch.model import Alert, Episode

from .matching import match
from .scoring import score


def grid(start: float, stop: float, step: float) -> list[float]:
    out: list[float] = []
    value = start
    # Accumulate in integer steps to keep the grid exact; a float accumulator
    # produces thresholds like 0.45000000000000007 in the report.
    n = 0
    while value <= stop + 1e-9:
        out.append(round(value, 6))
        n += 1
        value = round(start + n * step, 10)
    return out


def sweep_detector(
    detector: str,
    alerts: list[Alert],
    episodes: list[Episode],
    thresholds: Iterable[float],
    tape_seconds: float,
    market_makers: set[int],
    pre_ns: int,
    post_ns: int,
    weights: dict[str, float] | None = None,
    keep_penalty: bool = True,
) -> list[dict[str, Any]]:
    mine = [a for a in alerts if a.detector == detector]
    rescored = [(a, a.rescore(weights, keep_penalty)) for a in mine]
    points: list[dict[str, Any]] = []

    for t in thresholds:
        kept = [a for a, s in rescored if s >= t]
        result = match(kept, episodes, pre_ns, post_ns)
        summary = score(result, tape_seconds, market_makers, [detector])
        d = summary["detectors"].get(detector)
        if d is None:
            continue
        hours = summary["tape_hours"]
        mm_count = len(market_makers)
        points.append(
            {
                "threshold": t,
                "alerts": d["alerts"],
                "true_positives": d["true_positives"],
                "false_positives": d["false_positives"],
                "false_negatives": d["false_negatives"],
                "misclassified": d["misclassified"],
                "precision": d["precision"],
                "recall": d["recall"],
                "f1": d["f1"],
                "alerts_per_hour": d["alerts_per_hour"],
                "mm_false_positives": d["mm_false_positives"],
                "mm_fp_per_mm_hour": (
                    round(d["mm_false_positives"] / (mm_count * hours), 3)
                    if mm_count and hours
                    else 0.0
                ),
                "latency_p50_ms": d["latency_ms"]["p50"],
                "latency_p90_ms": d["latency_ms"]["p90"],
            }
        )
    return points


def pick_operating_point(points: list[dict[str, Any]], mm_budget: float) -> dict[str, Any]:
    if not points:
        return {"threshold": None, "reason": "no points"}

    affordable = [p for p in points if p["mm_fp_per_mm_hour"] <= mm_budget]
    if not affordable:
        best = min(points, key=lambda p: p["mm_fp_per_mm_hour"])
        return {
            "threshold": best["threshold"],
            "reason": "no threshold met the market-maker budget; "
                      "using the lowest false-positive rate available",
            "within_budget": False,
            "point": best,
        }

    # Ties on F1 go to the higher threshold: fewer alerts for the same score
    # is strictly better for the desk.
    best = max(affordable, key=lambda p: (round(p["f1"], 6), p["threshold"]))
    return {
        "threshold": best["threshold"],
        "reason": "highest F1 within the market-maker false-positive budget",
        "within_budget": True,
        "point": best,
    }


def run_sweep(
    alerts: list[Alert],
    episodes: list[Episode],
    detectors: Iterable[str],
    thresholds: list[float],
    tape_seconds: float,
    market_makers: set[int],
    pre_ns: int,
    post_ns: int,
    mm_budget: float,
) -> dict[str, Any]:
    out: dict[str, Any] = {"thresholds": thresholds, "mm_budget_per_mm_hour": mm_budget,
                           "detectors": {}}
    for name in detectors:
        points = sweep_detector(
            name, alerts, episodes, thresholds, tape_seconds, market_makers, pre_ns, post_ns
        )
        out["detectors"][name] = {
            "points": points,
            "operating_point": pick_operating_point(points, mm_budget),
        }
    return out


def apply_operating_points(alerts: list[Alert], sweep: dict[str, Any]) -> list[Alert]:
    """Filter a full alert set down to the chosen per-detector thresholds."""
    kept: list[Alert] = []
    for a in alerts:
        entry = sweep["detectors"].get(a.detector)
        if entry is None:
            kept.append(a)
            continue
        t = entry["operating_point"].get("threshold")
        if t is None or a.rescore() >= t:
            kept.append(a)
    return kept
