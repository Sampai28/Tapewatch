import os
import random
import tempfile
import unittest

from generate.book import BUY, SELL, MatchingBook
from generate.defects import apply_defects
from generate.main import build
from tapewatch import miniyaml, tape

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


class TestMatchingBook(unittest.TestCase):
    def setUp(self) -> None:
        self.b = MatchingBook()

    def test_price_time_priority(self) -> None:
        self.b.submit(1, 10, SELL, 101, 5, post_only=True)
        self.b.submit(2, 11, SELL, 101, 5, post_only=True)
        _, fills, resting = self.b.submit(3, 12, BUY, 101, 8)
        self.assertEqual(resting, 0)
        self.assertEqual([(f.maker_pid, f.qty) for f in fills], [(10, 5), (11, 3)])
        # The partially filled maker keeps its place with what is left.
        self.assertEqual(self.b.depth(SELL, 101), 2)

    def test_better_prices_trade_first(self) -> None:
        self.b.submit(1, 10, SELL, 103, 5, post_only=True)
        self.b.submit(1, 11, SELL, 101, 5, post_only=True)
        _, fills, _ = self.b.submit(2, 12, BUY, 103, 7)
        self.assertEqual([f.price for f in fills], [101, 103])

    def test_limit_price_stops_the_sweep(self) -> None:
        self.b.submit(1, 10, SELL, 101, 5, post_only=True)
        self.b.submit(1, 11, SELL, 105, 5, post_only=True)
        _, fills, resting = self.b.submit(2, 12, BUY, 102, 8)
        self.assertEqual(sum(f.qty for f in fills), 5)
        self.assertEqual(resting, 3)
        self.assertEqual(self.b.best_bid(), 102)

    def test_market_order_never_rests(self) -> None:
        self.b.submit(1, 10, SELL, 101, 2, post_only=True)
        _, fills, resting = self.b.submit(2, 11, BUY, None, 50)
        self.assertEqual(sum(f.qty for f in fills), 2)
        self.assertEqual(resting, 0)
        self.assertEqual(self.b.resting_count(), 0)

    def test_post_only_does_not_trade(self) -> None:
        self.b.submit(1, 10, SELL, 101, 5, post_only=True)
        _, fills, resting = self.b.submit(2, 11, BUY, 105, 5, post_only=True)
        self.assertEqual(fills, [])
        self.assertEqual(resting, 5)

    def test_cancel_removes_the_level_when_it_empties(self) -> None:
        oid, _, _ = self.b.submit(1, 10, BUY, 99, 5, post_only=True)
        self.assertEqual(self.b.best_bid(), 99)
        self.b.cancel(oid)
        self.assertIsNone(self.b.best_bid())
        self.assertEqual(self.b.resting_count(), 0)
        self.assertIsNone(self.b.cancel(oid))

    def test_self_trading_is_allowed(self) -> None:
        # The modelled venue has no self-trade prevention; without that,
        # direct wash trading never reaches the tape and there is nothing to
        # detect. See docs/notes.md.
        self.b.submit(1, 50, SELL, 101, 5, post_only=True)
        _, fills, _ = self.b.submit(2, 50, BUY, 101, 5)
        self.assertEqual(len(fills), 1)
        self.assertEqual(fills[0].maker_pid, 50)

    def test_reduce_keeps_priority(self) -> None:
        oid, _, _ = self.b.submit(1, 10, BUY, 99, 10, post_only=True)
        self.b.submit(2, 11, BUY, 99, 10, post_only=True)
        self.b.reduce(oid, 4)
        _, fills, _ = self.b.submit(3, 12, SELL, 99, 6)
        self.assertEqual([(f.maker_pid, f.qty) for f in fills], [(10, 4), (11, 2)])


class TestDefects(unittest.TestCase):
    def events(self, n: int) -> list[tape.Event]:
        return [
            tape.Event("O", ts=i * 1_000_000, seq=i + 1, eid=i + 1, symbol="TWX",
                       participant=1, order_id=i + 1, side="B", price=100, quantity=1)
            for i in range(n)
        ]

    def test_no_rates_means_no_change(self) -> None:
        evs = self.events(50)
        out, stats = apply_defects(evs, {}, random.Random(1))
        self.assertEqual(len(out), 50)
        self.assertEqual(stats["dropped"], 0)
        self.assertEqual(stats["duplicated"], 0)

    def test_drop_removes_events_and_leaves_a_sequence_gap(self) -> None:
        out, stats = apply_defects(self.events(200), {"drop_rate": 0.5}, random.Random(2))
        self.assertGreater(stats["dropped"], 0)
        self.assertEqual(len(out), 200 - stats["dropped"])
        seqs = [e.seq for e in out]
        self.assertNotEqual(seqs, list(range(1, len(seqs) + 1)))

    def test_duplicate_is_byte_identical(self) -> None:
        out, stats = apply_defects(self.events(100), {"duplicate_rate": 1.0}, random.Random(3))
        self.assertEqual(stats["duplicated"], 100)
        self.assertEqual(out[0].line(), out[1].line())

    def test_transpose_swaps_file_order_but_not_timestamps(self) -> None:
        out, stats = apply_defects(self.events(100), {"transpose_rate": 1.0}, random.Random(4))
        self.assertGreater(stats["transposed"], 0)
        self.assertEqual(sorted(e.ts for e in out), [e.ts for e in self.events(100)])
        self.assertNotEqual([e.ts for e in out], sorted(e.ts for e in out))

    def test_skew_never_produces_a_negative_timestamp(self) -> None:
        out, _ = apply_defects(self.events(100), {"skew_rate": 1.0, "skew_ms": 10_000},
                               random.Random(5))
        self.assertTrue(all(e.ts >= 0 for e in out))


class TestEndToEndGeneration(unittest.TestCase):
    """The tape has to be internally consistent, or every feed-integrity
    counter in a detection run measures the generator instead of the feed."""

    @classmethod
    def setUpClass(cls) -> None:
        cfg = miniyaml.load(os.path.join(ROOT, "configs", "smoke.yaml"))
        cfg["market"]["duration_seconds"] = 20
        cfg["feed_defects"] = {}
        cls.tmp = tempfile.TemporaryDirectory()
        cls.manifest = build(cfg, cls.tmp.name)
        cls.events = list(tape.read(os.path.join(cls.tmp.name, "tape.csv")))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_it_produced_a_tape(self) -> None:
        self.assertGreater(len(self.events), 200)
        self.assertGreater(self.manifest["episodes"], 0)

    def test_sequence_is_contiguous_without_defects(self) -> None:
        seqs = [e.seq for e in self.events]
        self.assertEqual(seqs, list(range(1, len(seqs) + 1)))

    def test_event_ids_are_unique(self) -> None:
        eids = [e.eid for e in self.events]
        self.assertEqual(len(set(eids)), len(eids))

    def test_timestamps_are_non_decreasing(self) -> None:
        for a, b in zip(self.events, self.events[1:]):
            self.assertLessEqual(a.ts, b.ts)

    def test_every_trade_references_a_live_order(self) -> None:
        live: dict[int, tuple[str, int]] = {}
        unknown = 0
        for e in self.events:
            if e.kind == "O":
                live[e.order_id] = (e.side, e.quantity)
            elif e.kind == "X":
                live.pop(e.order_id, None)
            elif e.kind == "M":
                if e.quantity > 0 and e.order_id in live:
                    live[e.order_id] = (e.side, e.quantity)
                else:
                    live.pop(e.order_id, None)
            elif e.kind == "T":
                entry = live.get(e.maker_order_id)
                if entry is None:
                    unknown += 1
                    continue
                side, qty = entry
                self.assertNotEqual(side, e.side, "maker must be on the opposite side")
                remaining = qty - e.quantity
                if remaining <= 0:
                    live.pop(e.maker_order_id)
                else:
                    live[e.maker_order_id] = (side, remaining)
        self.assertEqual(unknown, 0)

    def test_the_book_never_crosses(self) -> None:
        import sys

        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from check_tape import check  # noqa: E402

        crossed = check(os.path.join(self.tmp.name, "tape.csv"))
        self.assertEqual(crossed, 0)

    def test_ground_truth_windows_bound_real_behaviour(self) -> None:
        import json

        with open(os.path.join(self.tmp.name, "truth.json"), encoding="utf-8") as fh:
            truth = json.load(fh)
        first = self.events[0].ts
        last = self.events[-1].ts
        for ep in truth["episodes"]:
            self.assertLessEqual(ep["start_ts"], ep["end_ts"], ep)
            self.assertGreaterEqual(ep["start_ts"], first)
            self.assertLessEqual(ep["start_ts"], last)
            self.assertIn(ep["kind"],
                          ("spoofing", "layering", "wash_trading", "momentum_ignition"))
            self.assertGreaterEqual(ep["intensity"], 0.0)
            self.assertLessEqual(ep["intensity"], 1.0)

    def test_market_makers_are_not_manipulators(self) -> None:
        import json

        with open(os.path.join(self.tmp.name, "truth.json"), encoding="utf-8") as fh:
            truth = json.load(fh)
        self.assertGreater(len(truth["market_makers"]), 0)
        self.assertEqual(set(truth["market_makers"]) & set(truth["manipulators"]), set())
        # No episode may name a market maker, or the false positive rate
        # reported against them is measuring the generator's own mistake.
        mm = set(truth["market_makers"])
        for ep in truth["episodes"]:
            self.assertNotIn(ep["participant"], mm)

    def test_the_run_is_reproducible(self) -> None:
        cfg = miniyaml.load(os.path.join(ROOT, "configs", "smoke.yaml"))
        cfg["market"]["duration_seconds"] = 20
        cfg["feed_defects"] = {}
        with tempfile.TemporaryDirectory() as other:
            build(cfg, other)
            with open(os.path.join(other, "tape.csv"), encoding="utf-8") as a, \
                 open(os.path.join(self.tmp.name, "tape.csv"), encoding="utf-8") as b:
                self.assertEqual(a.read(), b.read())


if __name__ == "__main__":
    unittest.main()
