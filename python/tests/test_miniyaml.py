import unittest

from tapewatch import miniyaml


class TestMiniYaml(unittest.TestCase):
    def test_scalars(self) -> None:
        d = miniyaml.loads(
            """
            n: 42
            f: 0.25
            neg: -3
            t: true
            f2: False
            nil: null
            s: hello world
            q: "with: a colon"
            """
        )
        self.assertEqual(d["n"], 42)
        self.assertAlmostEqual(d["f"], 0.25)
        self.assertEqual(d["neg"], -3)
        self.assertIs(d["t"], True)
        self.assertIs(d["f2"], False)
        self.assertIsNone(d["nil"])
        self.assertEqual(d["s"], "hello world")
        self.assertEqual(d["q"], "with: a colon")

    def test_nested_maps(self) -> None:
        d = miniyaml.loads(
            """
            market:
              symbols: [A, B]
              depth:
                levels: 3
                size: 30
            top: 1
            """
        )
        self.assertEqual(d["market"]["symbols"], ["A", "B"])
        self.assertEqual(d["market"]["depth"]["levels"], 3)
        self.assertEqual(d["top"], 1)

    def test_inline_mapping(self) -> None:
        d = miniyaml.loads("weights: { size: 0.28, life: 0.18 }")
        self.assertAlmostEqual(d["weights"]["size"], 0.28)
        self.assertAlmostEqual(d["weights"]["life"], 0.18)

    def test_block_list_of_scalars_and_maps(self) -> None:
        d = miniyaml.loads(
            """
            names:
              - alpha
              - beta
            rules:
              - name: one
                n: 1
              - name: two
                n: 2
            """
        )
        self.assertEqual(d["names"], ["alpha", "beta"])
        self.assertEqual(d["rules"][0], {"name": "one", "n": 1})
        self.assertEqual(d["rules"][1]["n"], 2)

    def test_comments_and_blank_lines(self) -> None:
        d = miniyaml.loads(
            """
            # leading comment

            a: 1   # trailing comment
            b: 2
            """
        )
        self.assertEqual(d, {"a": 1, "b": 2})

    def test_empty_document(self) -> None:
        self.assertEqual(miniyaml.loads(""), {})
        self.assertEqual(miniyaml.loads("# only a comment\n"), {})

    def test_tabs_in_indentation_are_rejected(self) -> None:
        # A tab looks like indentation and is not, so a config with one would
        # parse into a different shape than it appears to have.
        with self.assertRaises(miniyaml.YamlError):
            miniyaml.loads("a:\n\tb: 1\n")

    def test_missing_colon_is_rejected(self) -> None:
        with self.assertRaises(miniyaml.YamlError):
            miniyaml.loads("just a line\n")

    def test_shipped_configs_parse(self) -> None:
        import os

        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
        for name in ("small.yaml", "full.yaml", "smoke.yaml"):
            path = os.path.join(root, "configs", name)
            if not os.path.exists(path):
                continue
            cfg = miniyaml.load(path)
            self.assertIn("market", cfg, name)
            self.assertIn("detectors", cfg, name)
            self.assertIn("evaluation", cfg, name)
            self.assertIn("spoofing", cfg["detectors"], name)
            self.assertIsInstance(cfg["detectors"]["spoofing"]["weights"], dict)


if __name__ == "__main__":
    unittest.main()
