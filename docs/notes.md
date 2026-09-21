# Design notes

Decisions that are not obvious from the code, and the reasoning behind them.
Where something is a compromise, it says so.

## Why there is a matching engine in the generator

A statistical event generator is much simpler and produces a file that looks
like market data until you reconstruct a book from it. Then every trade
references an order that never existed, depth goes negative, quotes cross
constantly — and every feed-integrity counter in the detection run ends up
measuring the generator instead of the feed.

So `python/generate/book.py` is a real, if small, price-time-priority
matching engine. `tools/check_tape.py` exists to prove the output is
internally consistent, and `python/tests/test_generator.py` runs it on every
test pass. On a clean tape the reconstructed book crosses zero times.

## Self-trade prevention is deliberately absent

The modelled venue lets a participant trade with itself. A venue with
self-trade prevention would reject those orders, direct wash trading would
never reach the tape, and the wash detector's clearest signal would be
untestable. That is a modelling choice, not an oversight — and a real
surveillance system on such a venue would rely much more heavily on the
matched-trade path.

## No third-party Python

Everything under `python/` uses the standard library. The pipeline has to run
on a bare `python3` with no pip, no virtualenv and no network, which is the
machine it was built on.

Three things cost more as a result:

- **`tapewatch/miniyaml.py`** — a YAML subset loader, about 150 lines, with a
  test file that pins what it does and does not support. PyYAML would be one
  line in a requirements file and one more reason the pipeline cannot run.
- **`tapewatch/svg.py`** — hand-rolled charts. The report needs a line chart,
  a scatter and a bar chart; matplotlib would be three lines each and a
  dependency.
- **SQLite rather than DuckDB.** SQLite is in the standard library. The query
  shapes here — fetch a page of alerts, join an alert to its episode, update
  a case — are transactional row lookups, which is what SQLite is for. DuckDB
  would be the right call if the console ran aggregate scans over months of
  alerts; it does not.

The tests are `unittest` for the same reason. `pyproject.toml` configures
pytest for anybody who prefers its runner, but nothing requires it.

## No Catch2

`tests/test_harness.hpp` is thirty lines: registration, an assertion that
prints both sides, and a non-zero exit code. Fetching a framework at build
time to get those would add a network dependency to the container build and
about forty seconds of compile time per translation unit, in exchange for
macros this suite does not use.

cpp-httplib *is* fetched, because writing an HTTP server is a different
proposition, and it is confined to the API target — `make build` and
`make test` work with nothing but a compiler.

## Detectors scan; they do not handle events

See `docs/detectors.md` for the reasoning. The consequence worth repeating
here is that detection latency has a floor of one `scan_interval_ms`, and
momentum ignition adds `burst_ms + revert_ms` on top because the evidence
that distinguishes it from informed trading does not exist until then. Both
are in the measured latency distribution.

## Scores are recorded, not just thresholds

Every alert carries its full signal vector and the weights that produced it.
This is what makes the threshold sweep and the weight ablation free: both are
pure functions of what the engine already wrote, so the whole
precision-recall curve costs one detection run and the curve cannot disagree
with the shipped configuration about what the detector computed.

The limit is that **gates cannot be ablated**. An alert that never fired
because it failed a hard gate is not in the file and cannot be recovered by
editing a weight. The ablation therefore measures each signal's contribution
to *ranking* among candidates that passed the gates.

## Crossed-book repair

A real venue's book is never crossed. If the reconstruction is, we are
holding an order the venue already removed and whose removal we never saw — a
dropped cancel or a dropped fill.

Left alone, one stale order inside the spread makes every depth, imbalance
and distance-from-touch measurement wrong for the rest of the session. On the
first run with feed defects enabled, six dropped messages produced 126,499
crossed observations out of 127,997 events: essentially the entire session
was measured against a book that did not exist.

`BookState::repair_cross()` discards the crossing level whose most recent
order is older, on the reasoning that the side nobody has refreshed is the
side we are out of date on. It is a heuristic and it can be wrong. What it
cannot be is silently wrong: every level it discards is counted and reported,
and on an undamaged feed it never fires — which the test suite asserts.

The proper fix is periodic book snapshots on the wire, which is what real
feeds do. The tape format has no snapshot record.

## The engine is never told who the market makers are

This is the single most important constraint in the project and it is worth
stating plainly. If the detectors could look up a market-maker roster, the
market-maker false positive rate would be measuring a lookup table rather
than a detector, and the headline number would be meaningless.

The roster is written into `truth.json` by the generator and read only by
`python/eval/`. The market-maker defences in the detectors — the two-sided
quoting penalty in spoofing and layering, the counterparty breadth penalty in
wash trading — infer market making from behaviour on the tape.

Both are defeatable by a manipulator willing to maintain token activity on
the other side or with other counterparties. That is the nature of a
behavioural inference.

## Where the alert/case boundary sits

The engine emits alerts. The console groups them into cases. Those are
different units and conflating them flatters the system: a detector that
fires six times on one episode has not found six things.

The evaluation counts duplicates separately from true positives for the same
reason — otherwise a detector could buy precision by firing repeatedly on the
cases it already found. The engine also suppresses repeats per
(detector, symbol, participant) within a cooldown, and the suppressed count
is reported rather than discarded, because alert volume is exactly the metric
suppression would quietly improve.

## Threshold selection is fitted, and that is a limitation

`python/eval/sweep.py` chooses each detector's operating point using the
ground truth: the highest-F1 threshold whose market-maker false positive rate
stays inside the configured budget. A real desk does not have the answer key.

So the tuned numbers are an optimistic bound. The **untuned** numbers — the
engine's own emission floor, no fitting at all — are reported next to them in
every table, and the gap between the two is the honest measure of how much of
the result is the detector and how much is the tuning.

## What the Matchbook adapter can and cannot do

`tools/matchbook_adapter.py` joins Matchbook's input stream (which has
participants) to its event log (which has sequence numbers and both sides of
every fill) on client order id.

Matchbook's log has **no timestamps** — deliberately, so that replays are
byte-identical across its four engine implementations. The adapter therefore
synthesises them from sequence position at a configured message rate. Every
duration a detector measures on an adapted tape — order lifetime, cancel
clustering, reversal windows — is consequently a function of that flag rather
than of anything that happened.

The adapter is for running real Matchbook flow through the detectors. It is
not for measuring them, which is why Tapewatch has its own generator.

## Things that are missing

- **Circular wash trading** (A → B → C → A). Needs cycle detection over a
  participant graph; not implemented.
- **Cross-symbol manipulation.** Each symbol has an independent book and
  independent detector state.
- **Hidden and iceberg liquidity.** Not in the tape, so nothing reasons about
  it. A real spoofing detector has to.
- **Book snapshots on the wire**, which would make a dropped message
  recoverable instead of permanent.
- **Authentication on the API.** The console writes triage state with no
  identity behind it. Fine for a laboratory, not for anything else.
- **Streaming.** Everything is batch over a finished tape. The detector
  design is streaming-shaped — bounded windows, bounded memory, one pass —
  but there is no live feed adapter.
