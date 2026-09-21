import unittest

from eval.matching import match
from eval.scoring import Counts, score, workload
from eval.sweep import grid, pick_operating_point
from tapewatch.model import Alert, Episode, percentile

SEC = 1_000_000_000
HOUR_TAPE = 3600.0


def alert(**over) -> Alert:
    base = dict(
        alert_id=1,
        detector="spoofing",
        symbol="TWX",
        participant=50,
        counterparty=None,
        start_ts=10 * SEC,
        end_ts=12 * SEC,
        detect_ts=13 * SEC,
        score=0.7,
        penalty=0.0,
        confidence=0.7,
        degraded=False,
        signals={"size": 1.0},
        weights={"size": 1.0},
        evidence={},
        order_ids=[],
    )
    base.update(over)
    return Alert(**base)  # type: ignore[arg-type]


def episode(**over) -> Episode:
    base = dict(
        episode_id=1,
        kind="spoofing",
        symbol="TWX",
        participant=50,
        start_ts=10 * SEC,
        end_ts=12 * SEC,
        intensity=0.8,
    )
    base.update(over)
    return Episode(**base)  # type: ignore[arg-type]


class TestCounts(unittest.TestCase):
    def test_precision_and_recall(self) -> None:
        c = Counts(true_positives=3, false_positives=1, false_negatives=2, episodes=5)
        self.assertAlmostEqual(c.precision, 0.75)
        self.assertAlmostEqual(c.recall, 0.6)
        self.assertAlmostEqual(c.f1, 2 * 0.75 * 0.6 / 1.35)

    def test_no_alerts_is_zero_not_a_crash(self) -> None:
        c = Counts(episodes=4)
        self.assertEqual(c.precision, 0.0)
        self.assertEqual(c.recall, 0.0)
        self.assertEqual(c.f1, 0.0)

    def test_lenient_precision_credits_a_mislabelled_hit(self) -> None:
        c = Counts(true_positives=1, misclassified=1, false_positives=2, episodes=2)
        self.assertAlmostEqual(c.precision, 1 / 3)
        self.assertAlmostEqual(c.lenient_precision, 2 / 4)


class TestScore(unittest.TestCase):
    def test_market_maker_false_positives_are_attributed(self) -> None:
        alerts = [
            alert(alert_id=1),                       # true positive
            alert(alert_id=2, participant=900),      # market maker false positive
            alert(alert_id=3, participant=901),      # someone else's false positive
        ]
        result = match(alerts, [episode()], 2 * SEC, 20 * SEC)
        s = score(result, HOUR_TAPE, {900}, ["spoofing"])
        self.assertEqual(s["overall"]["true_positives"], 1)
        self.assertEqual(s["overall"]["false_positives"], 2)
        self.assertEqual(s["market_maker"]["false_positives"], 1)
        self.assertAlmostEqual(s["market_maker"]["share_of_false_positives"], 0.5)
        self.assertAlmostEqual(s["market_maker"]["per_mm_per_hour"], 1.0)

    def test_a_wash_counterparty_that_is_a_market_maker_counts(self) -> None:
        alerts = [alert(alert_id=1, detector="wash_trading", participant=70, counterparty=900)]
        result = match(alerts, [], 2 * SEC, 20 * SEC)
        s = score(result, HOUR_TAPE, {900}, ["wash_trading"])
        self.assertEqual(s["market_maker"]["false_positives"], 1)

    def test_latency_is_measured_from_the_start_of_the_episode(self) -> None:
        result = match([alert(detect_ts=15 * SEC)], [episode(start_ts=10 * SEC)], 2 * SEC,
                       20 * SEC)
        s = score(result, HOUR_TAPE, set(), ["spoofing"])
        self.assertAlmostEqual(s["detectors"]["spoofing"]["latency_ms"]["p50"], 5000.0)

    def test_recall_is_bucketed_by_intensity(self) -> None:
        eps = [
            episode(episode_id=1, intensity=0.9),
            episode(episode_id=2, intensity=0.2, start_ts=100 * SEC, end_ts=101 * SEC),
        ]
        alerts = [alert(alert_id=1)]
        result = match(alerts, eps, 2 * SEC, 20 * SEC)
        s = score(result, HOUR_TAPE, set(), ["spoofing"])
        buckets = s["detectors"]["spoofing"]["recall_by_intensity"]
        self.assertEqual(buckets["high"]["recall"], 1.0)
        self.assertEqual(buckets["low"]["recall"], 0.0)

    def test_zero_length_tape_does_not_divide_by_zero(self) -> None:
        result = match([alert()], [episode()], 2 * SEC, 20 * SEC)
        s = score(result, 0.0, {900}, ["spoofing"])
        self.assertEqual(s["overall"]["alerts_per_hour"], 0.0)
        self.assertEqual(s["market_maker"]["per_mm_per_hour"], 0.0)


class TestWorkload(unittest.TestCase):
    def test_analyst_hours_follow_the_alert_rate(self) -> None:
        summary = {"overall": {"alerts_per_hour": 30.0}}
        w = workload(summary, triage_minutes=4.0, analyst_hours_per_day=7.5)
        self.assertAlmostEqual(w["analyst_hours_per_market_hour"], 2.0)
        self.assertAlmostEqual(w["alerts_per_analyst_shift"], 112.5)


class TestSweep(unittest.TestCase):
    def test_grid_is_exact(self) -> None:
        g = grid(0.25, 0.35, 0.025)
        self.assertEqual(g, [0.25, 0.275, 0.3, 0.325, 0.35])

    def test_operating_point_respects_the_market_maker_budget(self) -> None:
        points = [
            {"threshold": 0.3, "f1": 0.90, "mm_fp_per_mm_hour": 30.0},
            {"threshold": 0.6, "f1": 0.70, "mm_fp_per_mm_hour": 2.0},
            {"threshold": 0.9, "f1": 0.40, "mm_fp_per_mm_hour": 0.0},
        ]
        chosen = pick_operating_point(points, mm_budget=6.0)
        self.assertEqual(chosen["threshold"], 0.6)
        self.assertTrue(chosen["within_budget"])

    def test_no_affordable_point_is_reported_not_hidden(self) -> None:
        points = [
            {"threshold": 0.3, "f1": 0.9, "mm_fp_per_mm_hour": 40.0},
            {"threshold": 0.9, "f1": 0.5, "mm_fp_per_mm_hour": 12.0},
        ]
        chosen = pick_operating_point(points, mm_budget=6.0)
        self.assertFalse(chosen["within_budget"])
        self.assertIn("budget", chosen["reason"])
        self.assertEqual(chosen["threshold"], 0.9)

    def test_ties_on_f1_prefer_the_quieter_threshold(self) -> None:
        points = [
            {"threshold": 0.4, "f1": 0.8, "mm_fp_per_mm_hour": 1.0},
            {"threshold": 0.7, "f1": 0.8, "mm_fp_per_mm_hour": 0.5},
        ]
        self.assertEqual(pick_operating_point(points, 6.0)["threshold"], 0.7)


class TestPercentile(unittest.TestCase):
    def test_nearest_rank(self) -> None:
        data = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        self.assertEqual(percentile(data, 50), 5)
        self.assertEqual(percentile(data, 90), 9)
        self.assertEqual(percentile(data, 100), 10)
        self.assertEqual(percentile(data, 0), 1)

    def test_empty_is_zero(self) -> None:
        self.assertEqual(percentile([], 50), 0.0)

    def test_does_not_interpolate(self) -> None:
        # These are latencies quantised to the scan interval; a value between
        # two observations is one no alert had.
        self.assertIn(percentile([10, 20], 50), (10.0, 20.0))


if __name__ == "__main__":
    unittest.main()
