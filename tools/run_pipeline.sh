#!/usr/bin/env bash
# Generate, detect, evaluate and report in one pass.
#
#   tools/run_pipeline.sh configs/small.yaml results/small
#
# The Makefile calls the same four steps individually so that a single stage
# can be re-run; this exists for the times you want all of it.
set -euo pipefail

CONFIG="${1:-configs/small.yaml}"
RUN="${2:-results/small}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${TAPEWATCH_BIN:-$ROOT/build-native/tapewatch-detect}"
PY="${PYTHON:-python3}"

cd "$ROOT"
mkdir -p "$RUN"
export PYTHONPATH="$ROOT/python:${PYTHONPATH:-}"

echo "== config =="
"$PY" tools/mkdetcfg.py --config "$CONFIG" --out "$RUN/detectors.json"

echo "== generate =="
"$PY" -m generate.main --config "$CONFIG" --out "$RUN"

echo "== detect =="
"$BIN" --input "$RUN/tape.csv" --config "$RUN/detectors.json" \
       --alerts "$RUN/alerts.jsonl" --manifest "$RUN/detect.json"

echo "== evaluate =="
"$PY" -m eval.main --config "$CONFIG" --run "$RUN"

echo "== report =="
"$PY" -m eval.report --run "$RUN" --out "reports/$(basename "$RUN").html"
