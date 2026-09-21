"""Ablation: which signals are actually doing the work.

A composite score with five weighted components is a claim that all five
matter. Usually two of them do. This study tests the claim the cheap way --
by re-scoring the alerts the engine already emitted with one weight zeroed
and re-running the match at the same threshold.

Two limits, stated rather than glossed:

  The hard gates are not ablated. Spoofing requires an opposite-side trade,
  momentum ignition requires a reversal, layering requires a minimum number
  of levels; those are gates in the C++ detector, not weights, so an alert
  that never fired cannot be resurrected by editing a weight. What is
  measured is the contribution of each signal to *ranking* among candidates
  that passed the gates, which is what a threshold acts on.

  Zeroing a weight renormalises the rest, because the composite divides by
  the weight sum. Removing a signal therefore raises the scores of the
  remaining ones rather than just deleting a term, and recall can go *up*.
  That is the honest behaviour of this scoring rule and it is why the tables
  report the deltas in both directions.

The penalty terms -- two-sided quoting, counterparty breadth -- get their own
ablation, because they are the whole market-maker defence and it is worth
knowing exactly what they buy.
"""

from __future__ import annotations

from typing import Any, Iterable

from tapewatch.model import Alert, Episode

from .matching import match
from .scoring import score


def _evaluate(
    detector: str,
    alerts: list[Alert],
    episodes: list[Episode],
    threshold: float,
    tape_seconds: float,
    market_makers: set[int],
    pre_ns: int,
    post_ns: int,
    weights: dict[str, float] | None,
    keep_penalty: bool,
) -> dict[str, Any]:
    kept = [a for a in alerts if a.detector == detector and
            a.rescore(weights, keep_penalty) >= threshold]
    result = match(kept, episodes, pre_ns, post_ns)
    summary = score(result, tape_seconds, market_makers, [detector])
    d = summary["detectors"][detector]
    hours = summary["tape_hours"]
    mm_count = len(market_makers)
    return {
        "alerts": d["alerts"],
        "precision": d["precision"],
        "recall": d["recall"],
        "f1": d["f1"],
        "false_positives": d["false_positives"],
        "mm_false_positives": d["mm_false_positives"],
        "mm_fp_per_mm_hour": (
            round(d["mm_false_positives"] / (mm_count * hours), 3) if mm_count and hours else 0.0
        ),
    }


def run_ablation(
    alerts: list[Alert],
    episodes: list[Episode],
    detectors: Iterable[str],
    thresholds: dict[str, float],
    tape_seconds: float,
    market_makers: set[int],
    pre_ns: int,
    post_ns: int,
) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for name in detectors:
        threshold = thresholds.get(name)
        mine = [a for a in alerts if a.detector == name]
        if threshold is None or not mine:
            out[name] = {"threshold": threshold, "baseline": None, "signals": {}}
            continue

        weights = dict(mine[0].weights)
        baseline = _evaluate(name, alerts, episodes, threshold, tape_seconds, market_makers,
                             pre_ns, post_ns, None, True)

        signals: dict[str, Any] = {}
        for signal in sorted(weights):
            if weights[signal] == 0.0:
                continue
            modified = dict(weights)
            modified[signal] = 0.0
            if sum(modified.values()) <= 0:
                continue
            value = _evaluate(name, alerts, episodes, threshold, tape_seconds, market_makers,
                              pre_ns, post_ns, modified, True)
            signals[signal] = {
                "weight": weights[signal],
                **value,
                "delta_f1": round(value["f1"] - baseline["f1"], 4),
                "delta_precision": round(value["precision"] - baseline["precision"], 4),
                "delta_recall": round(value["recall"] - baseline["recall"], 4),
            }

        no_penalty = None
        if any(a.penalty > 0 for a in mine):
            value = _evaluate(name, alerts, episodes, threshold, tape_seconds, market_makers,
                              pre_ns, post_ns, None, False)
            no_penalty = {
                **value,
                "delta_f1": round(value["f1"] - baseline["f1"], 4),
                "delta_precision": round(value["precision"] - baseline["precision"], 4),
                "delta_recall": round(value["recall"] - baseline["recall"], 4),
                "delta_mm_false_positives": value["mm_false_positives"]
                - baseline["mm_false_positives"],
            }

        out[name] = {
            "threshold": threshold,
            "baseline": baseline,
            "signals": signals,
            "without_penalty": no_penalty,
        }
    return out
