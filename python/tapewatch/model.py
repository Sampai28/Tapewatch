"""The objects that cross the boundary between the C++ engine and the Python
harness: tape events, ground-truth episodes, and alerts.

Everything integer stays integer. Prices are minor units, quantities are
whole lots, timestamps are nanoseconds. The only floats in this file are
detector scores, which are never compared for equality.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from typing import Any, Iterable, Iterator

NS_PER_MS = 1_000_000
NS_PER_SEC = 1_000_000_000

BUY = "B"
SELL = "S"

DETECTORS = ("spoofing", "layering", "wash_trading", "momentum_ignition")

# Episode kinds map one-to-one onto detector names. Keeping them identical
# rather than mapping between two vocabularies means a typed match is a string
# comparison and cannot drift.
EPISODE_KINDS = DETECTORS


def other_side(side: str) -> str:
    return SELL if side == BUY else BUY


@dataclass
class Episode:
    """A piece of manipulation the generator deliberately injected.

    `order_ids` is what the manipulator actually sent, so a report can show
    the tape lines behind an episode. `intensity` records how blatant the
    generator made it, which is what the recall-by-intensity table is cut on:
    "we catch 95% of the obvious ones and 30% of the subtle ones" is a far
    more useful sentence than a single recall number.
    """

    episode_id: int
    kind: str
    symbol: str
    participant: int
    start_ts: int
    end_ts: int
    intensity: float
    counterparty: int | None = None
    order_ids: list[int] = field(default_factory=list)
    notes: dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "episode_id": self.episode_id,
            "kind": self.kind,
            "symbol": self.symbol,
            "participant": self.participant,
            "counterparty": self.counterparty,
            "start_ts": self.start_ts,
            "end_ts": self.end_ts,
            "intensity": round(self.intensity, 6),
            "order_ids": self.order_ids,
            "notes": self.notes,
        }

    @staticmethod
    def from_json(d: dict[str, Any]) -> "Episode":
        return Episode(
            episode_id=int(d["episode_id"]),
            kind=str(d["kind"]),
            symbol=str(d["symbol"]),
            participant=int(d["participant"]),
            counterparty=None if d.get("counterparty") is None else int(d["counterparty"]),
            start_ts=int(d["start_ts"]),
            end_ts=int(d["end_ts"]),
            intensity=float(d.get("intensity", 1.0)),
            order_ids=[int(x) for x in d.get("order_ids", [])],
            notes=dict(d.get("notes", {})),
        )


@dataclass
class Alert:
    """One alert as emitted by tapewatch-detect.

    `signals` and `weights` are carried through rather than collapsed into
    `score`, because the sweep and the ablation recompute the composite from
    them. There is one definition of the weighted sum that matters and it
    reads these fields; the C++ engine's copy is only ever used for the
    emission floor.
    """

    alert_id: int
    detector: str
    symbol: str
    participant: int
    counterparty: int | None
    start_ts: int
    end_ts: int
    detect_ts: int
    score: float
    penalty: float
    confidence: float
    degraded: bool
    signals: dict[str, float]
    weights: dict[str, float]
    evidence: dict[str, int]
    order_ids: list[int]

    @staticmethod
    def from_json(d: dict[str, Any]) -> "Alert":
        return Alert(
            alert_id=int(d["alert_id"]),
            detector=str(d["detector"]),
            symbol=str(d["symbol"]),
            participant=int(d["participant"]),
            counterparty=None if d.get("counterparty") is None else int(d["counterparty"]),
            start_ts=int(d["start_ts"]),
            end_ts=int(d["end_ts"]),
            detect_ts=int(d["detect_ts"]),
            score=float(d["score"]),
            penalty=float(d.get("penalty", 0.0)),
            confidence=float(d["confidence"]),
            degraded=bool(d["degraded"]),
            signals={k: float(v) for k, v in d.get("signals", {}).items()},
            weights={k: float(v) for k, v in d.get("weights", {}).items()},
            evidence={k: int(v) for k, v in d.get("evidence", {}).items()},
            order_ids=[int(x) for x in d.get("order_ids", [])],
        )

    def rescore(self, weights: dict[str, float] | None = None, keep_penalty: bool = True) -> float:
        """Recompute the composite, optionally with different weights.

        This is how the threshold sweep and the ablation study work. Both are
        pure functions of what the engine already recorded, so neither needs
        the engine re-run, and neither can disagree with it about what the
        signals were.
        """
        w = self.weights if weights is None else weights
        total = 0.0
        wsum = 0.0
        for name, value in self.signals.items():
            weight = w.get(name, 0.0)
            total += value * weight
            wsum += weight
        base = total / wsum if wsum > 0 else 0.0
        if keep_penalty:
            base -= self.penalty
        return min(1.0, max(0.0, base))

    def duration_ns(self) -> int:
        return max(0, self.end_ts - self.start_ts)


def load_alerts(path: str) -> list[Alert]:
    out: list[Alert] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line_no, line in enumerate(fh, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                out.append(Alert.from_json(json.loads(line)))
            except (ValueError, KeyError) as exc:
                raise ValueError(f"{path}:{line_no}: malformed alert: {exc}") from exc
    return out


def load_episodes(path: str) -> list[Episode]:
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    return [Episode.from_json(d) for d in data["episodes"]]


def load_truth(path: str) -> dict[str, Any]:
    """The whole ground-truth file: episodes plus the participant roster.

    The roster is the only place the market-maker identities live. The engine
    never sees this file.
    """
    with open(path, "r", encoding="utf-8") as fh:
        data = json.load(fh)
    data["episodes"] = [Episode.from_json(d) for d in data["episodes"]]
    return data


def iter_jsonl(path: str) -> Iterator[dict[str, Any]]:
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                yield json.loads(line)


def write_json(path: str, obj: Any) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2, sort_keys=False)
        fh.write("\n")


def percentile(values: Iterable[float], q: float) -> float:
    """Nearest-rank percentile on a sorted copy. Returns 0.0 for no data.

    Nearest-rank rather than interpolated: these are latencies measured in
    discrete scan intervals, and interpolating between two of them invents a
    value that no alert had.
    """
    data = sorted(values)
    if not data:
        return 0.0
    if q <= 0:
        return float(data[0])
    if q >= 100:
        return float(data[-1])
    rank = max(1, int(round(q / 100.0 * len(data))))
    return float(data[min(rank, len(data)) - 1])
