"""Build the SQLite database the console and the API server read.

SQLite rather than DuckDB for one reason: it is in the standard library, and
the whole Python side of this project runs without installing anything. The
query shapes here -- fetch a page of alerts, join an alert to its episode,
update a case -- are transactional row lookups, which is the workload SQLite
is for. A columnar store would be the right call if the console were doing
aggregate scans over months of alerts; it is not.

The database carries the *evaluation verdict* on each alert (true positive,
false positive, misclassified, duplicate) alongside the alert itself. That
is only possible because this is a laboratory: in production nothing knows
the answer at alert time. It is there so the console can show what a triage
queue looks like when you already know which items are noise, which is the
only way to look at a queue and judge whether the ranking is any good.
"""

from __future__ import annotations

import json
import os
import sqlite3
from typing import Any

from tapewatch.model import load_alerts, load_truth

SCHEMA = """
PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS participants (
    participant INTEGER PRIMARY KEY,
    role        TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS episodes (
    episode_id   INTEGER PRIMARY KEY,
    kind         TEXT    NOT NULL,
    symbol       TEXT    NOT NULL,
    participant  INTEGER NOT NULL,
    counterparty INTEGER,
    start_ts     INTEGER NOT NULL,
    end_ts       INTEGER NOT NULL,
    intensity    REAL    NOT NULL,
    detected     INTEGER NOT NULL DEFAULT 0,
    detect_ts    INTEGER,
    latency_ms   INTEGER,
    order_ids    TEXT    NOT NULL DEFAULT '[]',
    notes        TEXT    NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS alerts (
    alert_id     INTEGER PRIMARY KEY,
    detector     TEXT    NOT NULL,
    symbol       TEXT    NOT NULL,
    participant  INTEGER NOT NULL,
    counterparty INTEGER,
    start_ts     INTEGER NOT NULL,
    end_ts       INTEGER NOT NULL,
    detect_ts    INTEGER NOT NULL,
    score        REAL    NOT NULL,
    penalty      REAL    NOT NULL DEFAULT 0,
    confidence   REAL    NOT NULL,
    degraded     INTEGER NOT NULL DEFAULT 0,
    signals      TEXT    NOT NULL DEFAULT '{}',
    weights      TEXT    NOT NULL DEFAULT '{}',
    evidence     TEXT    NOT NULL DEFAULT '{}',
    order_ids    TEXT    NOT NULL DEFAULT '[]',
    episode_id   INTEGER,
    outcome      TEXT    NOT NULL DEFAULT 'false_positive',
    role         TEXT    NOT NULL DEFAULT 'unknown',
    above_operating_point INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_alerts_detector ON alerts(detector, score DESC);
CREATE INDEX IF NOT EXISTS idx_alerts_participant ON alerts(participant);
CREATE INDEX IF NOT EXISTS idx_alerts_detect_ts ON alerts(detect_ts);

-- Analyst state. Everything below this line is written by the console, not
-- by the pipeline, and survives a re-run of the evaluation.
CREATE TABLE IF NOT EXISTS alert_state (
    alert_id    INTEGER PRIMARY KEY,
    status      TEXT NOT NULL DEFAULT 'open',
    disposition TEXT,
    note        TEXT,
    updated_ts  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS cases (
    case_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    title      TEXT NOT NULL,
    status     TEXT NOT NULL DEFAULT 'open',
    assignee   TEXT,
    summary    TEXT,
    created_ts INTEGER NOT NULL DEFAULT 0,
    updated_ts INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS case_alerts (
    case_id  INTEGER NOT NULL,
    alert_id INTEGER NOT NULL,
    PRIMARY KEY (case_id, alert_id)
);
"""


def build_store(db_path: str, run_dir: str, evaluation: dict[str, Any]) -> str:
    truth = load_truth(os.path.join(run_dir, "truth.json"))
    alerts = load_alerts(os.path.join(run_dir, "alerts.jsonl"))

    # Rebuild the derived tables from scratch; leave analyst state alone so a
    # re-evaluation does not throw away someone's triage work.
    if os.path.exists(db_path):
        conn = sqlite3.connect(db_path)
        conn.executescript(SCHEMA)
        for table in ("alerts", "episodes", "participants", "meta"):
            conn.execute(f"DELETE FROM {table}")
    else:
        os.makedirs(os.path.dirname(os.path.abspath(db_path)), exist_ok=True)
        conn = sqlite3.connect(db_path)
        conn.executescript(SCHEMA)

    roster = {int(k): v for k, v in truth["roster"].items()}
    conn.executemany(
        "INSERT OR REPLACE INTO participants(participant, role) VALUES (?, ?)",
        sorted(roster.items()),
    )

    tuned = evaluation["tuned"]
    operating = evaluation["operating_points"]

    # Re-derive the per-alert verdict from the evaluation's own match, so the
    # console and the report cannot disagree about which alert was a miss.
    verdicts, episode_hit = _verdicts(evaluation, alerts, truth)

    conn.executemany(
        """INSERT OR REPLACE INTO episodes
           (episode_id, kind, symbol, participant, counterparty, start_ts, end_ts,
            intensity, detected, detect_ts, latency_ms, order_ids, notes)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)""",
        [
            (
                ep.episode_id,
                ep.kind,
                ep.symbol,
                ep.participant,
                ep.counterparty,
                ep.start_ts,
                ep.end_ts,
                ep.intensity,
                1 if episode_hit.get(ep.episode_id) else 0,
                episode_hit.get(ep.episode_id),
                (episode_hit[ep.episode_id] - ep.start_ts) // 1_000_000
                if episode_hit.get(ep.episode_id)
                else None,
                json.dumps(ep.order_ids),
                json.dumps(ep.notes),
            )
            for ep in truth["episodes"]
        ],
    )

    rows = []
    for a in alerts:
        verdict, episode_id = verdicts.get(a.alert_id, ("false_positive", None))
        threshold = operating.get(a.detector)
        above = 1 if (threshold is None or a.rescore() >= threshold) else 0
        rows.append(
            (
                a.alert_id, a.detector, a.symbol, a.participant, a.counterparty,
                a.start_ts, a.end_ts, a.detect_ts, a.score, a.penalty, a.confidence,
                1 if a.degraded else 0,
                json.dumps(a.signals), json.dumps(a.weights), json.dumps(a.evidence),
                json.dumps(a.order_ids), episode_id, verdict,
                roster.get(a.participant, "unknown"), above,
            )
        )
    conn.executemany(
        """INSERT OR REPLACE INTO alerts
           (alert_id, detector, symbol, participant, counterparty, start_ts, end_ts,
            detect_ts, score, penalty, confidence, degraded, signals, weights, evidence,
            order_ids, episode_id, outcome, role, above_operating_point)
           VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
        rows,
    )

    meta = {
        "run_dir": run_dir,
        "symbols": json.dumps(truth["symbols"]),
        "tape_seconds": str(evaluation["tape_seconds"]),
        "operating_points": json.dumps(operating),
        "precision": str(tuned["overall"]["precision"]),
        "recall": str(tuned["overall"]["recall"]),
        "f1": str(tuned["overall"]["f1"]),
        "alerts_per_hour": str(tuned["overall"]["alerts_per_hour"]),
        "mm_fp_per_mm_hour": str(tuned["market_maker"]["per_mm_per_hour"]),
        "market_makers": json.dumps(truth["market_makers"]),
    }
    conn.executemany("INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)",
                     sorted(meta.items()))
    conn.commit()
    conn.close()
    return db_path


def _verdicts(evaluation: dict[str, Any], alerts, truth):
    """Recompute the match so each alert row carries its own verdict."""
    from tapewatch.model import NS_PER_MS

    from .matching import match

    rule = evaluation["match_rule"]
    result = match(
        alerts,
        truth["episodes"],
        int(rule["pre_tolerance_ms"]) * NS_PER_MS,
        int(rule["post_tolerance_ms"]) * NS_PER_MS,
    )

    duplicate_ids = {a.alert_id for a in result.duplicates}
    verdicts: dict[int, tuple[str, int | None]] = {}
    for alert_id, episode_id in result.alert_to_episode.items():
        verdicts[alert_id] = (
            "duplicate" if alert_id in duplicate_ids else "true_positive",
            episode_id,
        )
    for alert_id, episode_id in result.alert_misclassified.items():
        verdicts[alert_id] = ("misclassified", episode_id)
    for a in result.false_positives:
        verdicts[a.alert_id] = ("false_positive", None)

    episode_hit: dict[int, int] = {}
    for ep in truth["episodes"]:
        first = result.first_hit(ep.episode_id)
        if first is not None:
            episode_hit[ep.episode_id] = first.detect_ts
    return verdicts, episode_hit
