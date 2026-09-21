import unittest

from eval.matching import match, overlaps
from tapewatch.model import Alert, Episode

SEC = 1_000_000_000


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
        signals={"size": 1.0, "opposite": 0.5},
        weights={"size": 0.5, "opposite": 0.5},
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


class TestOverlap(unittest.TestCase):
    def test_touching_windows_overlap(self) -> None:
        a = alert(start_ts=12 * SEC, end_ts=14 * SEC)
        self.assertTrue(overlaps(a, episode(), 0, 0))

    def test_gap_beyond_tolerance_does_not(self) -> None:
        a = alert(start_ts=30 * SEC, end_ts=31 * SEC)
        self.assertFalse(overlaps(a, episode(), 0, 5 * SEC))
        self.assertTrue(overlaps(a, episode(), 0, 20 * SEC))

    def test_pre_tolerance_is_separate_from_post(self) -> None:
        # An alert that fires before the behaviour is a coincidence, so the
        # window widens generously forwards and barely backwards.
        early = alert(start_ts=4 * SEC, end_ts=5 * SEC)
        self.assertFalse(overlaps(early, episode(), 2 * SEC, 20 * SEC))
        self.assertTrue(overlaps(early, episode(), 6 * SEC, 0))


class TestMatch(unittest.TestCase):
    def test_typed_match_is_a_true_positive(self) -> None:
        r = match([alert()], [episode()], 2 * SEC, 20 * SEC)
        self.assertEqual(len(r.episode_hits[1]), 1)
        self.assertEqual(r.false_positives, [])
        self.assertEqual(r.detected(), r.episodes)

    def test_wrong_detector_is_misclassified_not_a_false_positive(self) -> None:
        r = match([alert(detector="layering")], [episode()], 2 * SEC, 20 * SEC)
        self.assertEqual(r.episode_hits[1], [])
        self.assertEqual(len(r.episode_offtype[1]), 1)
        self.assertEqual(r.false_positives, [])
        self.assertEqual(r.missed(), r.episodes)

    def test_wrong_participant_is_a_false_positive(self) -> None:
        r = match([alert(participant=99)], [episode()], 2 * SEC, 20 * SEC)
        self.assertEqual(len(r.false_positives), 1)

    def test_wrong_symbol_never_matches(self) -> None:
        r = match([alert(symbol="OTHER")], [episode()], 2 * SEC, 1000 * SEC)
        self.assertEqual(len(r.false_positives), 1)

    def test_either_side_of_a_wash_pair_counts(self) -> None:
        ep = episode(kind="wash_trading", participant=50, counterparty=51)
        a = alert(detector="wash_trading", participant=51, counterparty=50)
        r = match([a], [ep], 2 * SEC, 20 * SEC)
        self.assertEqual(len(r.episode_hits[1]), 1)

        # ...and an unrelated pair still does not.
        b = alert(detector="wash_trading", participant=70, counterparty=71)
        r2 = match([b], [ep], 2 * SEC, 20 * SEC)
        self.assertEqual(len(r2.false_positives), 1)

    def test_second_alert_on_one_episode_is_a_duplicate(self) -> None:
        r = match(
            [alert(alert_id=1), alert(alert_id=2, detect_ts=14 * SEC)],
            [episode()],
            2 * SEC,
            20 * SEC,
        )
        self.assertEqual(len(r.episode_hits[1]), 2)
        self.assertEqual([a.alert_id for a in r.duplicates], [2])
        # Duplicates are not false positives; they cost time, not accuracy.
        self.assertEqual(r.false_positives, [])

    def test_earliest_alert_is_the_detection(self) -> None:
        r = match(
            [alert(alert_id=9, detect_ts=20 * SEC), alert(alert_id=3, detect_ts=13 * SEC)],
            [episode()],
            2 * SEC,
            20 * SEC,
        )
        first = r.first_hit(1)
        assert first is not None
        self.assertEqual(first.alert_id, 3)

    def test_overlapping_episodes_credit_the_earlier_one(self) -> None:
        eps = [
            episode(episode_id=1, start_ts=10 * SEC, end_ts=12 * SEC),
            episode(episode_id=2, start_ts=11 * SEC, end_ts=13 * SEC),
        ]
        r = match([alert()], eps, 2 * SEC, 20 * SEC)
        self.assertEqual(len(r.episode_hits[1]), 1)
        self.assertEqual(r.episode_hits[2], [])

    def test_an_alert_is_credited_at_most_once(self) -> None:
        eps = [
            episode(episode_id=1, start_ts=10 * SEC, end_ts=12 * SEC),
            episode(episode_id=2, start_ts=10 * SEC, end_ts=12 * SEC),
        ]
        r = match([alert()], eps, 2 * SEC, 20 * SEC)
        credited = sum(len(v) for v in r.episode_hits.values())
        self.assertEqual(credited, 1)


class TestRescore(unittest.TestCase):
    def test_rescore_reproduces_the_engine(self) -> None:
        a = alert(signals={"x": 1.0, "y": 0.0}, weights={"x": 0.5, "y": 0.5}, penalty=0.0)
        self.assertAlmostEqual(a.rescore(), 0.5)

    def test_zeroing_a_weight_renormalises(self) -> None:
        # Removing a signal does not just delete a term; it raises what is
        # left, which is why an ablation can increase recall.
        a = alert(signals={"x": 1.0, "y": 0.0}, weights={"x": 0.5, "y": 0.5})
        self.assertAlmostEqual(a.rescore({"x": 0.5, "y": 0.0}), 1.0)

    def test_penalty_can_be_ablated(self) -> None:
        a = alert(signals={"x": 1.0}, weights={"x": 1.0}, penalty=0.4)
        self.assertAlmostEqual(a.rescore(), 0.6)
        self.assertAlmostEqual(a.rescore(keep_penalty=False), 1.0)

    def test_score_stays_in_range(self) -> None:
        a = alert(signals={"x": 1.0}, weights={"x": 1.0}, penalty=5.0)
        self.assertAlmostEqual(a.rescore(), 0.0)


if __name__ == "__main__":
    unittest.main()
