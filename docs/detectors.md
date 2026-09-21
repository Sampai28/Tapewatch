# The detectors

Four patterns, four detectors, one shape. Each one takes a window of the
tape, computes several signals in `[0, 1]`, combines them as a weighted mean,
subtracts a penalty where there is one, and fires above a threshold. What
differs is the evidence and, more importantly, the **gates**: the conditions
that have to hold before any scoring happens at all.

The gates matter more than the weights. A weight decides how an alert ranks;
a gate decides whether the alert exists. Most of the false positive reduction
in this system comes from three gates, not from tuning.

## Scoring

```
score = ( Σ wᵢ · sᵢ ) / ( Σ wᵢ )  −  penalty
```

Signals are clamped to `[0, 1]`. Weights are normalised by their sum, so a
configuration whose weights do not add to one still produces a score in
range — editing one weight changes the ranking without silently shifting
every operating point. Penalties are kept separate from the weighted sum and
emitted on the alert, so the Python harness can re-derive the score, sweep
the threshold, and ablate either the weights or the penalty without re-running
the engine.

Every alert carries its full signal vector. That is what makes the sweep and
the ablation cost nothing, and it is what an analyst reads first: a spoofing
alert scoring 0.7 entirely on order size is a different object from one
scoring 0.7 on the opposite-side trade, and only the second is worth opening.

## Scanning, not handling

All four detectors are scanners. The pipeline advances the book event by
event and then, every `scan_interval_ms` of tape time, asks each detector to
look at the slice that just became available.

Three reasons. Cost: an event handler that walks a thirty-second trade window
on every trade is quadratic in the message rate, while a scanner is linear.
Correctness: some of these patterns are only visible after the fact, and
there is no event at which the answer exists. And it is what surveillance
desks actually do — nobody runs a wash-trade test on every print.

The cost is that detection latency has a floor of one scan interval, and two
of the detectors add a much larger deliberate delay on top. Both are in the
measured latency numbers rather than argued away.

---

## Spoofing

Post a large order on one side with no intention of trading it, let the
display of size move other participants, pull it, and take liquidity on the
other side at the price your own order helped make.

Recognising a large order that gets cancelled quickly is not the hard part.
That is what a market maker does several hundred times an hour, legitimately.

**Gates**

| gate | why |
|---|---|
| cancelled quantity ≥ `min_qty` | an order too small to move anyone cannot have been a display |
| placed within `near_touch_ticks` of the touch | measured at placement, not at cancel; fifty ticks away nobody was looking |
| lifetime ≤ 3 × `max_lifetime_ms` | keeps long-lived inventory orders out; the curve does the ranking |
| **aggressive** opposite-side execution > 0 | the load-bearing one — see below |

**Signals**

| signal | weight | what it measures |
|---|---:|---|
| `size` | 0.28 | order quantity against the mean resting order size on that side, captured *before* the order joined it |
| `life` | 0.18 | how fast it was pulled, against `max_lifetime_ms` |
| `unfilled` | 0.12 | fraction of the order that never traded |
| `opposite` | 0.30 | aggressive quantity the participant took the other way, against the size of the pulled order |
| `press` | 0.12 | closeness to the touch at placement |

**Penalty:** `two_sided_penalty` (0.45) × how balanced the participant's
placements were across both sides during the window.

### The two things that make this work

**Only aggressive opposite-side volume counts.** A spoofer pulls a bid and
then *sells aggressively* — taking the offer was the point and the bid was
the advertisement. A market maker pulls a bid and is then *lifted on its own
offer*: passive, chosen by somebody else, and no more evidence of intent than
the weather. Counting both was worth several hundred false positives per
session against market makers on the shipped config, and essentially nothing
in recall.

**Two-sided quoting is penalised heavily.** Symmetric quoting is the
signature of market making and asymmetry is the whole point of spoofing. At
0.35 the penalty was not enough: a market maker running the same shape at
ordinary size still cleared the threshold. It is the setting to lower first
if recall matters more than precision.

The engine is **never** told which participants are registered market makers.
If it were, the market-maker false positive rate — the number this project
exists to report — would be measuring a lookup table. The roster lives in the
generator's ground truth and is read only by the evaluation harness.

---

## Layering

Spoofing's plural: a stack of orders across several price levels on one side.
Harder to spot per-order, and a more convincing wall, because depth spread
over levels reads as genuine interest rather than one participant's bluff.

The tell is **coordination**. Real interest at five price levels arrives and
leaves independently, because the reasons for it are independent. A layer
arrives together and leaves together — one decision cancels all of it, within
a fraction of a second, about when the participant trades the other way.

**Gates**: ≥ `min_orders` cancels by one participant, on one side, inside
`cancel_span_ms`; across ≥ `min_levels` distinct prices; all within
`near_touch_ticks` at placement; and aggressive opposite-side execution > 0.

**Signals**

| signal | weight | what it measures |
|---|---:|---|
| `levels` | 0.24 | distinct price levels in the cluster |
| `imbalance` | 0.24 | the layer's quantity as a share of that side of the book, against the depth that was there before it |
| `coord` | 0.18 | how tightly the cancels grouped inside `cancel_span_ms` |
| `opposite` | 0.22 | aggressive quantity taken the other way |
| `unfilled` | 0.12 | fraction of the layer that never traded |

**Penalty:** `two_sided_penalty` (0.30), same mechanism as spoofing.

Layering and spoofing both fire on the same behaviour when a layer contains
one dominant order. That is left alone rather than suppressed. The evaluation
reports typed matches, so a layering episode caught only by the spoofing
detector shows up as a miss for layering — which is information — and the
console groups alerts by participant and time, so an analyst sees one case.

---

## Wash trading

Trading with yourself. The point is the print: volume that looks like
interest, a price that looks like a market, and no change in who owns what.

Three shapes; this detector handles two:

- **direct** — the same participant on both sides. Trivial to spot, trivial
  to avoid, and therefore rare in practice.
- **matched** — two accounts trading back and forth in near-equal size. The
  reciprocity is the tell.
- **circular** — the same through a chain of three or more. **Not detected.**
  See the limitations below.

**Gates**: gross quantity in the window ≥ `min_gross_qty`; and either a
self-trade, or a pair with non-zero volume in *both* directions. One-sided
flow between two accounts is a position, not a wash.

**Signals**

| signal | weight | what it measures |
|---|---:|---|
| `self` | 0.34 | fraction of the participant's volume that was self-traded |
| `recip` | 0.26 | `min(A→B, B→A) / max(A→B, B→A)` |
| `nonet` | 0.16 | how close the pair's combined net position change is to zero |
| `share` | 0.16 | the pair's volume as a fraction of the busier party's total |
| `price` | 0.08 | fraction of prints more than `offmarket_ticks` from the prevailing mid |

**Penalty:** `breadth_penalty` (0.45) × how many distinct counterparties the
busier party traded with, against `breadth_target`.

### The market maker problem, again

Over any window a market maker has large gross volume, near-zero net
position, and trades both ways with the same active counterparties.
Reciprocity, flatness and repetition — three of the five signals — describe a
market maker perfectly, and no amount of tuning those three separates the
two.

What separates them is **concentration**, in two forms, and the detector uses
both. `share` asks how much of the busier party's volume went through this
one counterparty: near all of it for a wash pair, a small slice for a market
maker. `breadth` asks how many counterparties they had at all, and is
subtracted rather than added — the same shape as the two-sided defence in
spoofing, and inferred from behaviour rather than from a roster.

---

## Momentum ignition

Buy aggressively enough, fast enough, that momentum traders and stop orders
follow, then sell into the move you started. The manipulation is not the
buying — that is just trading — it is the buying *in order to* sell.

Which means intent has to be inferred from what happens afterwards, and that
has a consequence the detector cannot design around. **At the moment of the
burst, momentum ignition is indistinguishable from an informed trader who has
decided the price is wrong and is taking the offer until it is right.** They
look identical for several seconds. They differ in two things that follow:

- the informed trader keeps the position, and the price stays moved
- the igniter reverses out, and the price comes back

So the detector waits. It evaluates a burst only once `revert_ms` of tape has
passed since the burst ended, because before then the answer does not exist.
The cost is a detection latency floor of roughly `burst_ms + revert_ms` —
about ten seconds on the shipped config — and it is in the measured numbers.
Compare with spoofing, where the discriminating evidence arrives within a
window of the cancel and detection is correspondingly quicker.

**Gates**: midpoint moved ≥ `min_move_ticks` across the burst window;
≥ `min_orders` **distinct aggressive orders** in the direction of the move;
the participant's burst quantity ≥ `min_share` of all aggressive volume in
the window; and reverse-direction volume ≥ `min_reverse_fraction` of the
burst.

Two of those were learned the hard way. Counting **fills** rather than
distinct orders made "a burst of four" the description of one ordinary market
order sweeping five levels, and every uninformed taker on the venue cleared
the bar. And a reversal gate of "greater than zero" is met by anybody who
trades in both directions occasionally; it has to be a fraction.

**Signals**

| signal | weight | what it measures |
|---|---:|---|
| `burst` | 0.18 | distinct aggressive orders, against `burst_target_orders` |
| `move` | 0.22 | midpoint movement in ticks, against `move_target_ticks` |
| `share` | 0.18 | the participant's share of aggressive volume during the burst |
| `reverse` | 0.26 | quantity taken back the other way, against the burst |
| `revert` | 0.16 | how much of the move gave itself back within `revert_ms` |

No penalty term. The gates carry the market-maker defence here, and an
inventory-flattening market maker still gets through some of them — which is
visible in the measured false positive counts rather than hidden.

---

## Known limitations

- **Circular wash trading is not detected.** A → B → C → A nets to nothing
  and no single pair is reciprocal. Detecting it means looking for cycles in
  a participant graph over the window, which is a different algorithm from
  everything else here and is not implemented.
- **Cross-symbol manipulation is not detected.** Each symbol has an
  independent book and independent detector state. Ramping one instrument to
  move a correlated one is invisible.
- **Iceberg and hidden liquidity are not modelled.** The tape has no hidden
  quantity, so nothing here reasons about it. A real venue's spoofing
  detector has to.
- **The two-sided and breadth penalties can be gamed** by a manipulator who
  maintains token activity on the other side or with other counterparties.
  Both are behavioural inferences, and behavioural inferences are defeatable
  by behaviour.
- **Momentum ignition against a genuinely informed trader who happens to take
  profit** is a false positive this design cannot avoid. The difference is
  intent, and intent is not on the tape.
- **Thresholds are fitted on the same synthetic distribution they are
  evaluated on.** The sweep chooses an operating point using ground truth;
  the numbers in the README are therefore an optimistic bound, and the
  untuned figures are reported alongside them for that reason.
