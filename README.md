# Tapewatch

Market abuse surveillance. A C++20 streaming detection engine for spoofing,
layering, wash trading and momentum ignition; a Python harness that generates
synthetic order flow with injected ground truth and scores the detectors
against it; and a React console for triaging what comes out.

The interesting part is not detecting manipulation in a market made of
manipulators. It is detecting it in a market full of **legitimate
participants who look guilty** — and reporting honestly how often you get
them wrong.

## The false positive problem

A market maker cancels hundreds of orders an hour, quotes both sides, holds
an enormous order-to-trade ratio, and when inventory runs away from it, pulls
the quotes on one side and crosses the spread to flatten. Pull the bids, then
sell aggressively, within a second, never having been filled on the orders
that were pulled: that is the textbook description of spoofing, performed by
somebody doing nothing wrong.

An informed trader who has decided the price is wrong takes the offer until
it is right. For the first several seconds that is indistinguishable from
momentum ignition. The difference is what happens next, and it has not
happened yet.

The generator produces both, along with momentum followers, noise traders and
uninformed takers, and the engine is **never told which participants are
which**. The roster lives in the ground truth and is read only by the
evaluation harness — otherwise the market-maker false positive rate would be
measuring a lookup table.

## Measured results

Default configuration (`configs/small.yaml`): 15 minutes of tape, one symbol,
38 participants, 80,747 events, 133 injected episodes. Feed defects on.
Numbers are from `results/small/eval.json`; the full report with curves is
`reports/small.html`.

**Untuned** is the engine's own emission floor, with no fitting at all.
**Tuned** applies the per-detector operating point chosen by the threshold
sweep. The sweep picks its point using ground truth, which a real desk does
not have, so the tuned column is an optimistic bound and both are shown.

| detector | episodes | untuned P / R | threshold | tuned P / R | F1 | latency p50 |
|---|---:|---|---:|---|---:|---:|
| spoofing | 43 | 0.212 / 0.930 | 0.575 | **0.976 / 0.930** | 0.952 | 3,981 ms |
| layering | 33 | 0.129 / 0.636 | 0.550 | **0.875 / 0.636** | 0.737 | 5,408 ms |
| wash trading | 31 | 1.000 / 1.000 | 0.575 | **1.000 / 1.000** | 1.000 | 253 ms |
| momentum ignition | 26 | 0.643 / 0.346 | 0.500 | **0.643 / 0.346** | 0.450 | 8,494 ms |
| **overall** | **133** | **0.254 / 0.759** | | **0.918 / 0.759** | **0.831** | 3,968 ms |

### Market-maker false positives

| | untuned | tuned |
|---|---:|---:|
| false positives naming a market maker | 296 | **9** |
| share of all false positives | 100% | 100% |
| per market maker per hour | 296.0 | **9.0** |

Every false positive in the run is a market maker. Nobody else trips these
detectors at all — not the informed traders, not the momentum followers, not
the noise. That is the shape of the problem stated as precisely as this
project can state it: the false positive load is not spread across the
venue, it is concentrated entirely on the participants least likely to be
manipulating and most expensive to keep annoying.

### What it costs a desk

| | untuned | tuned |
|---|---:|---:|
| alerts | 429 | 140 |
| alerts per hour | 1,716 | 560 |
| analyst-hours per market hour (4 min/alert) | 114.4 | **37.3** |

A further 3,777 alerts were suppressed by per-participant cooldowns inside
the engine and never reached the evaluation. They are counted rather than
discarded, because alert volume is precisely the metric suppression would
otherwise quietly improve.

37 analyst-hours per market hour is still not deployable. It is what one
symbol of synthetic flow with four market makers costs at this operating
point, and it is the number a real deployment would have to attack next — by
grouping alerts into cases, which the console does, and by whitelisting
registered market makers, which this project deliberately refuses to do
because it would make the headline result meaningless.

### Recall against how blatant the abuse was

Every injected episode carries an intensity in [0, 1] controlling order size,
how fast the manipulator pulls, and how much of the move it takes.

| detector | low (<0.5) | medium | high (>0.75) |
|---|---|---|---|
| spoofing | 0.91 (10/11) | 0.94 (16/17) | 0.93 (14/15) |
| layering | 0.41 (7/17) | 0.82 (9/11) | 1.00 (5/5) |
| wash trading | 1.00 (11/11) | 1.00 (8/8) | 1.00 (12/12) |
| momentum ignition | 0.20 (1/5) | 0.36 (5/14) | 0.43 (3/7) |

Layering has a clear floor: below intensity 0.5 the layers are three orders
of modest size and more than half of them are missed. Momentum ignition is
weak everywhere, for a reason that is structural rather than a tuning
failure — see below.

### Engine throughput

80,747 events in 0.33 s, **244,542 events/second**, single-threaded, `-O2`,
g++ 13.3.0 under WSL2 on an AMD Ryzen 7 260. 1,861 detector scans over the
run. Memory is bounded by configuration, not by tape length: every rolling
window has a fixed capacity, and window overflows are counted (zero in this
run).

### Feed integrity

The generator damages the tape on purpose. This run: 6 dropped messages, 6
duplicates, 17 transposed pairs, 12 skewed timestamps.

| counter | value |
|---|---:|
| duplicate event ids dropped | 6 |
| sequence gaps (48 events implied missing) | 15 |
| timestamp/sequence conflicts | 42 |
| out-of-order events recovered by the reorder buffer | 24 |
| out-of-order events dropped as unrecoverable | 0 |
| stale book levels repaired after a lost message | 5 |
| alerts marked degraded | 38 |

Six dropped messages, before crossed-book repair existed, produced 126,499
crossed observations out of 127,997 events on an earlier run: one stale order
inside the spread makes every depth and distance measurement wrong for the
rest of the session. `BookState::repair_cross()` discards the stale side and
counts every level it throws away. On a clean feed it never fires, and the
test suite asserts that.

## What the ablation says

Zeroing one signal's weight and re-scoring at the same threshold. Gates are
not ablated — an alert that never fired cannot be recovered by editing a
weight — so this measures each signal's contribution to *ranking*.

| detector | most load-bearing signal | ΔF1 without it |
|---|---|---:|
| spoofing | `size` | −0.883 |
| layering | `coord` | −0.524 |
| wash trading | `recip` | −0.348 |
| momentum ignition | `reverse` | −0.172 |

And the market-maker defences, which are penalties rather than weights:

| detector | without the penalty | ΔF1 | MM false positives |
|---|---|---:|---:|
| spoofing | two-sided quoting | −0.608 | 1 → **149** |
| layering | two-sided quoting | −0.506 | 3 → **128** |
| wash trading | counterparty breadth | 0.000 | 0 → 0 |

Removing the two-sided quoting penalty costs nothing in recall and multiplies
market-maker false positives by roughly 150× and 40×. It is doing more work
than any individual signal. The wash detector's breadth penalty changes
nothing on this tape, because the market makers never form a reciprocal
concentrated pair — it is insurance, and the run shows it idle.

## Where it is weakest

**Momentum ignition: 0.64 precision, 0.35 recall.** This is not a tuning
problem. At the moment of the burst, an igniter and an informed trader are
the same observation; they differ only in whether the position comes back off
and the price reverts, which is information that does not exist yet. The
detector therefore waits `burst_ms + revert_ms` before judging anything,
which puts its median detection latency at 8.5 seconds against wash
trading's 0.25, and the gates that keep ordinary takers out (a minimum share
of aggressive flow, a minimum reversal fraction) also exclude the quieter
half of the real episodes. All five of its false positives are market makers
flattening inventory, which is the same observation from the other side.

**Layering recall at low intensity is 0.41.** A three-order layer of modest
size inside a busy book does not produce enough imbalance to separate from a
market maker refreshing a three-level quote.

**Thresholds are fitted on the distribution they are evaluated on.** The
untuned column exists because of this and should be read as the floor.

**Sample sizes are small.** 26 to 43 episodes per detector. `configs/full.yaml`
runs three symbols over four hours for when that matters; nothing in this
README comes from it.

## Running it

```bash
make all
```

Builds the engine, runs the C++ and Python suites, generates a tape, detects,
evaluates and writes `reports/small.html`. Needs a C++20 compiler and
`python3`, and nothing else — no pip, no virtualenv, no network. On two cores
the whole pipeline is a few minutes; detection itself is a third of a second.

The console:

```bash
make all && make up
```

- console — <http://localhost:5180>
- API — <http://localhost:8090/api/health>

`make down` stops it. The containers never generate data; `make all` writes
`results/small/{tape.csv,tapewatch.db}` on the host and the stack reads them,
so the numbers on screen and the numbers in the report come from the same run
by construction.

Individual stages: `make generate`, `make detect`, `make eval`, `make report`,
each taking `CONFIG=` and `RUN=`. `make ui-test` runs the console's suite,
`make docker-test` runs everything inside the container.

## The console

React 18 and TypeScript in strict mode, Vite, no state library. A triage
queue ranked by confidence — score discounted where the feed was damaged,
not raw score; an evidence pane showing every signal's contribution so an
analyst can see *why* something fired before deciding whether to open it;
book replay reconstructed from the tape with the alert's own orders
highlighted, because dismissing a spoofing alert means looking at the book;
and case management, since six alerts on one participant over four minutes
are one investigation.

It also shows the ground truth for each alert, clearly labelled as something
no real desk has. A ranking is only as good as what sits at the top of it,
and that cannot be judged without knowing which items are noise.

## Layout

The detection engine is C++ because it is the streaming hot path: bounded
windows, bounded memory, one pass, integer prices throughout. The evaluation
is Python because it is analysis — matching, scoring, sweeps, ablations,
report generation — and none of it is on the hot path.

```
include/tapewatch/   engine headers: events, book, windows, detectors, config
src/engine/          tape parsing, book reconstruction, the pipeline, the CLI
src/detectors/       the four detectors, one file each
src/validation/      reorder buffer and feed-integrity accounting
src/api/             REST API over SQLite, plus book replay
python/generate/     matching engine, participants, manipulators, defects
python/eval/         matching, scoring, sweep, ablation, store, report
ui/                  the console
tests/  python/tests/  ui/src/**/*.test.ts
```

`docs/detectors.md` is the detector specification: gates, signals, weights,
and what each defence is for. `docs/notes.md` covers the design decisions and
their costs.

## Relationship to Matchbook

Tapewatch reads a normalised tape that is a strict superset of what
[Matchbook](../Matchbook) writes, and `tools/matchbook_adapter.py` converts a
Matchbook run into one. That conversion is lossier than it looks: Matchbook's
event log has no timestamps at all — deliberately, so replays are
byte-identical across its four engine implementations — and no participant on
output records, while its input stream has participants but no time and no
symbol. The adapter joins the two on client order id and synthesises
timestamps from sequence position at a configured rate.

Every duration a detector measures on an adapted tape is therefore a function
of that flag rather than of anything that happened, which is exactly why
Tapewatch has its own generator and why no measurement here depends on
Matchbook at all.

## Known limitations

- **Circular wash trading** (A → B → C → A) is not detected: no pair is
  reciprocal and the group nets to nothing.
- **Cross-symbol manipulation** is not detected. Each symbol has an
  independent book and independent detector state.
- **Hidden and iceberg liquidity** are not modelled, so nothing reasons about
  them. A real spoofing detector has to.
- **The behavioural market-maker defences are defeatable** by a manipulator
  willing to maintain token activity on the other side, or with other
  counterparties. That is the nature of a behavioural inference.
- **Detection latency has a floor** of one scan interval (500 ms), and
  momentum ignition adds `burst_ms + revert_ms` on top by construction.
- **A dropped message is unrecoverable.** The repair heuristic keeps the book
  usable; the proper fix is periodic snapshots on the wire, which the tape
  format does not have.
- **The API has no authentication.** The console writes triage state with no
  identity behind it.
- **Everything is batch** over a finished tape. The detectors are
  streaming-shaped, but there is no live feed adapter.
