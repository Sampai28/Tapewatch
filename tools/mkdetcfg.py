#!/usr/bin/env python3
"""Extract the `detectors:` block from a run config into JSON for the engine.

The C++ side reads JSON because it has a JSON parser and no business having a
YAML one. The human edits YAML because the run config also holds the market
model and the evaluation settings, and three files would drift. This is the
seam between the two, and it is twenty lines rather than a format.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))

from tapewatch import miniyaml  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)

    cfg = miniyaml.load(args.config)
    detectors = cfg.get("detectors")
    if not isinstance(detectors, dict):
        print(f"mkdetcfg: {args.config} has no `detectors:` block", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(detectors, fh, indent=2)
        fh.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
