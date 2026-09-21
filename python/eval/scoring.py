"""Turning a match result into the numbers a compliance team would ask for.

Precision and recall are table stakes and they are not the interesting part.
Three other numbers decide whether a surveillance system is usable:

  detection latency     how long after the abuse began the alert existed.
                        An alert that arrives an hour late is an audit
                        trail. Reported as a distribution, not a mean,
                        because the tail is what an operating procedure has
                        to be written against.
  alerts per hour       and, converted through a triage cost, how many
                        analyst-hours per market-hour the system demands. A
                        detector with 95% recall that needs four analysts is
                        not deployable.
  market-maker FP rate  how much of the false positive load lands on the
                        participants most likely to look guilty and least
                        likely to be. Reported prominently, per market maker
                        per hour, because it is the number that gets a
                        surveillance system switched off.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable

from tapewatch.model import NS_PER_MS, percentile

from .matching import MatchResult


@dataclass
class Counts:
    true_positives: int = 0
    false_negatives: int = 0
    false_positives: int = 0
    misclassified: int = 0
    duplicates: int = 0
    alerts: int = 0
    episodes: int = 0

    @property
    def precision(self) -> float:
        denom = self.true_positives + self.false_positives
        return self.true_positives / denom if denom else 0.0

    @property
    def lenient_precision(self) -> float:
        """Credit an alert that found real abuse but labelled it wrongly."""
        hits = self.true_positives + self.misclassified
        denom = hits + self.false_positives
        return hits / denom if denom else 0.0

    @property
    def recall(self) -> float:
        return self.true_positives / self.episodes if self.episodes else 0.0

    @property
    def f1(self) -> float:
        p, r = self.precision, self.recall
        return 2 * p * r / (p + r) if (p + r) > 0 else 0.0


@dataclass
class DetectorScore:
    detector: str
    counts: Counts
    latency_ms: dict[str, float] = field(default_factory=dict)
    alerts_per_hour: float = 0.0
    recall_by_intensity: dict[str, dict[str, float]] = field(default_factory=dict)
    mm_false_positives: int = 0

    def to_json(self) -> dict[str, Any]:
        c = self.counts
        return {
            "detector": self.detector,
            "episodes": c.episodes,
            "alerts": c.alerts,
            "true_positives": c.true_positives,
            "false_positives": c.false_positives,
            "false_negatives": c.false_negatives,
            "misclassified": c.misclassified,
            "duplicates": c.duplicates,
            "precision": round(c.precision, 4),
            "lenient_precision": round(c.lenient_precision, 4),
            "recall": round(c.recall, 4),
            "f1": round(c.f1, 4),
            "latency_ms": {k: round(v, 1) for k, v in self.latency_ms.items()},
            "alerts_per_hour": round(self.alerts_per_hour, 2),
            "mm_false_positives": self.mm_false_positives,
            "recall_by_intensity": self.recall_by_intensity,
        }


INTENSITY_BUCKETS = (("low", 0.0, 0.5), ("medium", 0.5, 0.75), ("high", 0.75, 1.01))


def _bucket(intensity: float) -> str:
    for name, lo, hi in INTENSITY_BUCKETS:
        if lo <= intensity < hi:
            return name
    return "high"


def _latency_stats(values: list[float]) -> dict[str, float]:
    if not values:
        return {"n": 0, "p50": 0.0, "p90": 0.0, "p99": 0.0, "max": 0.0, "min": 0.0}
    return {
        "n": len(values),
        "min": min(values),
        "p50": percentile(values, 50),
        "p90": percentile(values, 90),
        "p99": percentile(values, 99),
        "max": max(values),
    }


def score(
    result: MatchResult,
    tape_seconds: float,
    market_makers: Iterable[int],
    detectors: Iterable[str],
) -> dict[str, Any]:
    mm = set(market_makers)
    hours = tape_seconds / 3600.0 if tape_seconds > 0 else 0.0

    per_detector: dict[str, DetectorScore] = {}
    for name in detectors:
        per_detector[name] = DetectorScore(name, Counts())

    for ep in result.episodes:
        ds = per_detector.setdefault(ep.kind, DetectorScore(ep.kind, Counts()))
        ds.counts.episodes += 1
        bucket = ds.recall_by_intensity.setdefault(
            _bucket(ep.intensity), {"episodes": 0, "detected": 0}
        )
        bucket["episodes"] += 1
        if result.episode_hits.get(ep.episode_id):
            ds.counts.true_positives += 1
            bucket["detected"] += 1
        else:
            ds.counts.false_negatives += 1

    latencies: dict[str, list[float]] = {name: [] for name in per_detector}
    for ep in result.episodes:
        first = result.first_hit(ep.episode_id)
        if first is None:
            continue
        latencies.setdefault(ep.kind, []).append(
            (first.detect_ts - ep.start_ts) / NS_PER_MS
        )

    for alert in result.alerts:
        ds = per_detector.setdefault(alert.detector, DetectorScore(alert.detector, Counts()))
        ds.counts.alerts += 1
    for alert in result.duplicates:
        per_detector[alert.detector].counts.duplicates += 1
    for alert in result.false_positives:
        ds = per_detector[alert.detector]
        ds.counts.false_positives += 1
        if alert.participant in mm or (alert.counterparty is not None
                                       and alert.counterparty in mm):
            ds.mm_false_positives += 1
    for alert_id, _ in result.alert_misclassified.items():
        alert = next(a for a in result.alerts if a.alert_id == alert_id)
        per_detector[alert.detector].counts.misclassified += 1

    for name, ds in per_detector.items():
        ds.latency_ms = _latency_stats(latencies.get(name, []))
        ds.alerts_per_hour = ds.counts.alerts / hours if hours > 0 else 0.0
        for bucket in ds.recall_by_intensity.values():
            bucket["recall"] = (
                round(bucket["detected"] / bucket["episodes"], 4) if bucket["episodes"] else 0.0
            )

    overall = Counts()
    for ds in per_detector.values():
        overall.true_positives += ds.counts.true_positives
        overall.false_positives += ds.counts.false_positives
        overall.false_negatives += ds.counts.false_negatives
        overall.misclassified += ds.counts.misclassified
        overall.duplicates += ds.counts.duplicates
        overall.alerts += ds.counts.alerts
        overall.episodes += ds.counts.episodes

    all_latencies = [v for values in latencies.values() for v in values]
    mm_fp = sum(ds.mm_false_positives for ds in per_detector.values())
    mm_count = len(mm)

    return {
        "tape_seconds": round(tape_seconds, 3),
        "tape_hours": round(hours, 4),
        "overall": {
            "episodes": overall.episodes,
            "alerts": overall.alerts,
            "true_positives": overall.true_positives,
            "false_positives": overall.false_positives,
            "false_negatives": overall.false_negatives,
            "misclassified": overall.misclassified,
            "duplicates": overall.duplicates,
            "precision": round(overall.precision, 4),
            "lenient_precision": round(overall.lenient_precision, 4),
            "recall": round(overall.recall, 4),
            "f1": round(overall.f1, 4),
            "latency_ms": {k: round(v, 1) for k, v in _latency_stats(all_latencies).items()},
            "alerts_per_hour": round(overall.alerts / hours, 2) if hours else 0.0,
        },
        "market_maker": {
            "count": mm_count,
            "false_positives": mm_fp,
            "share_of_false_positives": (
                round(mm_fp / overall.false_positives, 4) if overall.false_positives else 0.0
            ),
            "per_mm_per_hour": (
                round(mm_fp / (mm_count * hours), 3) if mm_count and hours else 0.0
            ),
        },
        "detectors": {name: ds.to_json() for name, ds in sorted(per_detector.items())},
    }


def workload(summary: dict[str, Any], triage_minutes: float,
             analyst_hours_per_day: float) -> dict[str, Any]:
    """What this alert volume costs in people.

    Precision is an abstraction; this is the number a head of compliance
    actually decides on.
    """
    per_hour = summary["overall"]["alerts_per_hour"]
    analyst_hours_per_market_hour = per_hour * triage_minutes / 60.0
    return {
        "triage_minutes_per_alert": triage_minutes,
        "alerts_per_hour": per_hour,
        "analyst_hours_per_market_hour": round(analyst_hours_per_market_hour, 3),
        "analysts_required": round(analyst_hours_per_market_hour, 2),
        "alerts_per_analyst_shift": round(analyst_hours_per_day * 60.0 / triage_minutes, 1)
        if triage_minutes > 0
        else 0.0,
    }
