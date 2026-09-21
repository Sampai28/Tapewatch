import io
import unittest

from tapewatch import tape


class TestTapeFormat(unittest.TestCase):
    def test_order_round_trip(self) -> None:
        e = tape.Event("O", ts=123, seq=4, eid=5, symbol="TWX", participant=7,
                       order_id=9, side="S", price=-250, quantity=500)
        back = tape.parse(e.line())
        self.assertEqual(back, e)

    def test_trade_carries_both_participants(self) -> None:
        e = tape.Event("T", ts=1, seq=2, eid=3, symbol="A", participant=11, order_id=21,
                       maker_participant=12, maker_order_id=22, side="B",
                       price=10050, quantity=3)
        back = tape.parse(e.line())
        assert back is not None
        self.assertEqual(back.participant, 11)
        self.assertEqual(back.maker_participant, 12)
        self.assertEqual(back.maker_order_id, 22)

    def test_cancel_is_seven_fields(self) -> None:
        e = tape.Event("X", ts=1, seq=2, eid=3, symbol="A", participant=4, order_id=5)
        self.assertEqual(len(e.line().split(",")), 7)
        self.assertEqual(tape.parse(e.line()), e)

    def test_comments_and_blanks_are_skipped(self) -> None:
        self.assertIsNone(tape.parse(""))
        self.assertIsNone(tape.parse("   "))
        self.assertIsNone(tape.parse("# a header"))

    def test_wrong_field_count_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            tape.parse("O,1,2,3,TWX,4,5")
        with self.assertRaises(ValueError):
            tape.parse("X,1,2,3,TWX,4,5,6")

    def test_unknown_kind_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            tape.parse("Q,1,2,3,TWX,4,5,B,1,1")

    def test_writer_numbers_events_contiguously(self) -> None:
        buf = io.StringIO()
        w = tape.Writer(buf)
        for i in range(5):
            w.write(tape.Event("X", ts=i, seq=0, eid=0, symbol="A", participant=1, order_id=i))
        lines = buf.getvalue().strip().split("\n")
        seqs = [int(line.split(",")[2]) for line in lines]
        eids = [int(line.split(",")[3]) for line in lines]
        self.assertEqual(seqs, [1, 2, 3, 4, 5])
        self.assertEqual(eids, [1, 2, 3, 4, 5])
        self.assertEqual(w.count, 5)


if __name__ == "__main__":
    unittest.main()
