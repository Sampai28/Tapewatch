import json
import os
import sqlite3
import tempfile
import unittest

from eval.store import build_store
from tapewatch.model import write_json

SEC = 1_000_000_000


def write_run(run_dir: str) -> None:
    truth = {
        "symbols": ["TWX"],
        "seed": 1,
        "duration_seconds": 60.0,
        "tick_size": 1,
        "roster": {"900": "market_maker", "50": "spoofer", "51": "noise"},
        "market_makers": [900],
        "manipulators": [50],
        "information_events": [],
        "episodes": [
            {
                "episode_id": 1,
                "kind": "spoofing",
                "symbol": "TWX",
                "participant": 50,
                "counterparty": None,
                "start_ts": 10 * SEC,
                "end_ts": 12 * SEC,
                "intensity": 0.8,
                "order_ids": [7],
                "notes": {},
            }
        ],
    }
    write_json(os.path.join(run_dir, "truth.json"), truth)

    alerts = [
        {  # true positive
            "alert_id": 1, "detector": "spoofing", "symbol": "TWX", "participant": 50,
            "counterparty": None, "start_ts": 10 * SEC, "end_ts": 12 * SEC,
            "detect_ts": 13 * SEC, "score": 0.8, "penalty": 0.0, "confidence": 0.8,
            "degraded": False, "signals": {"size": 1.0}, "weights": {"size": 1.0},
            "evidence": {"cancelled_qty": 200}, "order_ids": [7],
        },
        {  # duplicate on the same episode
            "alert_id": 2, "detector": "spoofing", "symbol": "TWX", "participant": 50,
            "counterparty": None, "start_ts": 11 * SEC, "end_ts": 12 * SEC,
            "detect_ts": 14 * SEC, "score": 0.6, "penalty": 0.0, "confidence": 0.6,
            "degraded": False, "signals": {"size": 0.6}, "weights": {"size": 1.0},
            "evidence": {}, "order_ids": [],
        },
        {  # market maker false positive, below the operating point
            "alert_id": 3, "detector": "spoofing", "symbol": "TWX", "participant": 900,
            "counterparty": None, "start_ts": 40 * SEC, "end_ts": 41 * SEC,
            "detect_ts": 42 * SEC, "score": 0.3, "penalty": 0.45, "confidence": 0.3,
            "degraded": True, "signals": {"size": 0.3}, "weights": {"size": 1.0},
            "evidence": {}, "order_ids": [],
        },
    ]
    with open(os.path.join(run_dir, "alerts.jsonl"), "w", encoding="utf-8") as fh:
        for a in alerts:
            fh.write(json.dumps(a) + "\n")


EVALUATION = {
    "tape_seconds": 60.0,
    "match_rule": {"pre_tolerance_ms": 2000, "post_tolerance_ms": 20000},
    "operating_points": {"spoofing": 0.5},
    "tuned": {
        "overall": {"precision": 1.0, "recall": 1.0, "f1": 1.0, "alerts_per_hour": 60.0},
        "market_maker": {"per_mm_per_hour": 0.0},
    },
}


class TestStore(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        write_run(self.tmp.name)
        self.db = os.path.join(self.tmp.name, "tapewatch.db")
        build_store(self.db, self.tmp.name, EVALUATION)
        self.conn = sqlite3.connect(self.db)

    def tearDown(self) -> None:
        self.conn.close()
        self.tmp.cleanup()

    def q(self, sql: str, *args):
        return self.conn.execute(sql, args).fetchall()

    def test_alerts_carry_their_verdict(self) -> None:
        rows = dict(self.q("SELECT alert_id, outcome FROM alerts"))
        self.assertEqual(rows[1], "true_positive")
        self.assertEqual(rows[2], "duplicate")
        self.assertEqual(rows[3], "false_positive")

    def test_roles_come_from_the_roster(self) -> None:
        rows = dict(self.q("SELECT alert_id, role FROM alerts"))
        self.assertEqual(rows[1], "spoofer")
        self.assertEqual(rows[3], "market_maker")

    def test_operating_point_flag(self) -> None:
        rows = dict(self.q("SELECT alert_id, above_operating_point FROM alerts"))
        self.assertEqual(rows[1], 1)
        self.assertEqual(rows[2], 1)
        self.assertEqual(rows[3], 0)

    def test_episode_records_its_detection(self) -> None:
        detected, detect_ts, latency = self.q(
            "SELECT detected, detect_ts, latency_ms FROM episodes WHERE episode_id=1"
        )[0]
        self.assertEqual(detected, 1)
        self.assertEqual(detect_ts, 13 * SEC)
        self.assertEqual(latency, 3000)

    def test_json_columns_stay_json(self) -> None:
        signals, evidence = self.q(
            "SELECT signals, evidence FROM alerts WHERE alert_id=1"
        )[0]
        self.assertEqual(json.loads(signals), {"size": 1.0})
        self.assertEqual(json.loads(evidence), {"cancelled_qty": 200})

    def test_rebuilding_preserves_analyst_state(self) -> None:
        # Re-running the evaluation must refresh the alerts without throwing
        # away somebody's triage work.
        self.conn.execute(
            "INSERT INTO alert_state(alert_id, status, disposition, note, updated_ts) "
            "VALUES (1, 'closed', 'market making', 'checked', 123)"
        )
        self.conn.execute(
            "INSERT INTO cases(case_id, title, status, created_ts, updated_ts) "
            "VALUES (1, 'a case', 'open', 1, 1)"
        )
        self.conn.execute("INSERT INTO case_alerts(case_id, alert_id) VALUES (1, 1)")
        self.conn.commit()
        self.conn.close()

        build_store(self.db, self.tmp.name, EVALUATION)
        self.conn = sqlite3.connect(self.db)

        self.assertEqual(self.q("SELECT status FROM alert_state WHERE alert_id=1")[0][0],
                         "closed")
        self.assertEqual(self.q("SELECT count(*) FROM cases")[0][0], 1)
        self.assertEqual(self.q("SELECT count(*) FROM case_alerts")[0][0], 1)
        self.assertEqual(self.q("SELECT count(*) FROM alerts")[0][0], 3)

    def test_meta_carries_the_headline_numbers(self) -> None:
        meta = dict(self.q("SELECT key, value FROM meta"))
        self.assertEqual(meta["precision"], "1.0")
        self.assertEqual(json.loads(meta["market_makers"]), [900])
        self.assertEqual(json.loads(meta["operating_points"]), {"spoofing": 0.5})


if __name__ == "__main__":
    unittest.main()
