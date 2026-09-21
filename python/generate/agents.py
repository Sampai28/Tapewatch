"""Legitimate participants.

These exist to be *false positives*. A surveillance system evaluated against a
tape containing only manipulators and random noise will report a precision
that means nothing, because the hard cases are absent. Each agent here is
modelled around a specific way of looking guilty while doing nothing wrong:

  MarketMaker       enormous order-to-trade ratio, constant cancels, quotes
                    on both sides, and -- because it skews on inventory --
                    it moves the price after absorbing a large trade. Its
                    cancel pattern is the spoofing and layering confounder.
  InformedTrader    aggresses hard in one direction and moves the price. For
                    the first several seconds it is indistinguishable from
                    momentum ignition. It differs in not reversing.
  MomentumFollower  buys strength and sells weakness, which means it is
                    always aggressing in the direction of a recent move --
                    exactly where an ignition detector is looking.
  NoiseTrader       posts and pulls constantly at ordinary size.
  Taker             uninformed marketable flow; the volume everything else
                    trades against.

The engine is never told which participant is which. The roster is written
into the ground-truth file and read only by the evaluation harness.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class Agent:
    pid: int
    role: str
    next_ts: int = 0

    def act(self, sim) -> None:  # pragma: no cover - interface
        raise NotImplementedError


@dataclass
class MarketMaker(Agent):
    levels: int = 3
    base_size: int = 30
    half_spread: int = 2
    level_gap: int = 1
    reprice_ticks: int = 1
    interval_ms: int = 120
    inventory: int = 0
    # Ticks of quote shift per unit of inventory. This is what makes the price
    # move after a large trade and come back when the position is unwound,
    # which is the mechanism the momentum-ignition scenario relies on. Without
    # it a burst has no lasting effect and there is nothing to find.
    skew_per_lot: float = 0.02
    max_inventory: int = 400
    # When the position gets past this, the maker stops being a maker: it
    # pulls the quotes that would add to the position and crosses the spread
    # to flatten.
    #
    # This is the hardest false positive in the whole system and it is not an
    # edge case -- every market maker does it several times a session. Pull
    # the bids, then sell aggressively, in under a second, having never been
    # filled on the orders that were pulled. That is the textbook description
    # of spoofing, performed by someone doing nothing wrong. Leaving it out
    # of the generator would make the spoofing detector look far better than
    # it is.
    flatten_at: int = 220
    flatten_clip: int = 90
    quotes: dict[str, list[int]] = field(default_factory=lambda: {"B": [], "S": []})
    last_center: int | None = None

    def center(self, sim) -> int:
        skew = int(round(self.skew_per_lot * self.inventory))
        return sim.fair - skew

    def act(self, sim) -> None:
        if abs(self.inventory) >= self.flatten_at:
            self._flatten(sim)
            return
        center = self.center(sim)
        if self.last_center is not None and abs(center - self.last_center) < self.reprice_ticks:
            # Quotes are still good. Refresh one random level anyway, which is
            # what keeps a real market maker's order-to-trade ratio high.
            self._refresh_one(sim)
            self.next_ts = sim.ts + sim.jitter(self.interval_ms)
            return

        self._pull_all(sim)
        self._post_all(sim, center)
        self.last_center = center
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)

    def _flatten(self, sim) -> None:
        long = self.inventory > 0
        adding_side = "B" if long else "S"
        for oid in self.quotes[adding_side]:
            sim.cancel(self.pid, oid)
            sim.advance_micro(sim.rng.randint(200_000, 900_000))
        self.quotes[adding_side] = []
        reduce_side = "S" if long else "B"
        qty = min(abs(self.inventory), self.flatten_clip)
        sim.market(self.pid, reduce_side, qty)
        self.last_center = None  # requote from scratch next time round
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)

    def _pull_all(self, sim) -> None:
        for side in ("B", "S"):
            for oid in self.quotes[side]:
                sim.cancel(self.pid, oid)
            self.quotes[side] = []

    def _post_all(self, sim, center: int) -> None:
        # Stop quoting the side that would make the position worse. A market
        # maker at its limit going one-sided is ordinary risk management, and
        # it is also the single most common reason a two-sided-quoting defence
        # fails to protect somebody who deserves protecting.
        quote_bid = self.inventory < self.max_inventory
        quote_ask = self.inventory > -self.max_inventory
        for i in range(self.levels):
            size = max(1, int(self.base_size * sim.rng.uniform(0.6, 1.4)))
            if quote_bid:
                px = center - self.half_spread - i * self.level_gap
                oid = sim.place(self.pid, "B", px, size, post_only=True)
                if oid is not None:
                    self.quotes["B"].append(oid)
            if quote_ask:
                px = center + self.half_spread + i * self.level_gap
                oid = sim.place(self.pid, "S", px, size, post_only=True)
                if oid is not None:
                    self.quotes["S"].append(oid)

    def _refresh_one(self, sim) -> None:
        side = "B" if sim.rng.random() < 0.5 else "S"
        if not self.quotes[side]:
            return
        idx = sim.rng.randrange(len(self.quotes[side]))
        oid = self.quotes[side][idx]
        order = sim.book.orders.get(oid)
        if order is None:
            self.quotes[side].pop(idx)
            return
        sim.cancel(self.pid, oid)
        self.quotes[side].pop(idx)
        size = max(1, int(self.base_size * sim.rng.uniform(0.6, 1.4)))
        new_oid = sim.place(self.pid, side, order.price, size, post_only=True)
        if new_oid is not None:
            self.quotes[side].append(new_oid)

    def on_fill(self, side: str, qty: int) -> None:
        # `side` is the maker's own direction.
        self.inventory += qty if side == "B" else -qty


@dataclass
class NoiseTrader(Agent):
    interval_ms: int = 900
    size: int = 12
    max_resting: int = 4
    offset_ticks: int = 4
    resting: list[int] = field(default_factory=list)

    def act(self, sim) -> None:
        self.resting = [o for o in self.resting if o in sim.book.orders]
        if self.resting and (len(self.resting) >= self.max_resting or sim.rng.random() < 0.45):
            idx = sim.rng.randrange(len(self.resting))
            oid = self.resting[idx]
            order = sim.book.orders.get(oid)
            # Sometimes trim instead of pulling. Venues let participants amend
            # size down without losing priority, and a detector that only
            # watches cancels has an obvious hole if nobody ever does it.
            if order is not None and order.qty > 2 and sim.rng.random() < 0.3:
                sim.amend(self.pid, oid, order.qty // 2)
            else:
                self.resting.pop(idx)
                sim.cancel(self.pid, oid)
        else:
            side = "B" if sim.rng.random() < 0.5 else "S"
            ref = sim.book.best(side)
            if ref is None:
                ref = sim.fair + (-2 if side == "B" else 2)
            off = sim.rng.randint(0, self.offset_ticks)
            px = ref - off if side == "B" else ref + off
            qty = max(1, int(self.size * sim.rng.uniform(0.5, 1.6)))
            oid = sim.place(self.pid, side, px, qty, post_only=True)
            if oid is not None:
                self.resting.append(oid)
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)


@dataclass
class Taker(Agent):
    interval_ms: int = 1500
    size: int = 15

    def act(self, sim) -> None:
        side = "B" if sim.rng.random() < 0.5 else "S"
        qty = max(1, int(self.size * sim.rng.uniform(0.4, 1.8)))
        sim.market(self.pid, side, qty)
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)


@dataclass
class MomentumFollower(Agent):
    interval_ms: int = 1200
    size: int = 18
    lookback_ms: int = 3000
    trigger_ticks: int = 2

    def act(self, sim) -> None:
        move = sim.mid_move(self.lookback_ms)
        if move is not None and abs(move) >= self.trigger_ticks:
            side = "B" if move > 0 else "S"
            qty = max(1, int(self.size * sim.rng.uniform(0.6, 1.5)))
            sim.market(self.pid, side, qty)
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)


@dataclass
class InformedTrader(Agent):
    """Occasionally learns something true and trades on it.

    The fair value moves first and this agent chases it; the price ends up
    where the information says it should and stays there. Every observable
    during the burst matches momentum ignition. The absence of a reversal is
    the only difference, which is exactly the point.
    """

    interval_ms: int = 40000
    burst_orders: int = 6
    burst_gap_ms: int = 120
    size: int = 25
    jump_ticks: int = 8

    _burst_left: int = 0
    _burst_side: str = "B"

    def act(self, sim) -> None:
        if self._burst_left > 0:
            qty = max(1, int(self.size * sim.rng.uniform(0.7, 1.3)))
            sim.market(self.pid, self._burst_side, qty)
            self._burst_left -= 1
            # When the burst is done, wait for the next piece of information.
            # Rescheduling at burst_gap_ms here instead makes this agent
            # aggress without pause for the whole session, which swamps every
            # other participant's share of the aggressive flow.
            self.next_ts = sim.ts + sim.jitter(
                self.burst_gap_ms if self._burst_left > 0 else self.interval_ms
            )
            return

        direction = 1 if sim.rng.random() < 0.5 else -1
        jump = direction * sim.rng.randint(self.jump_ticks // 2, self.jump_ticks)
        sim.fair += jump
        sim.note_information_event(self.pid, sim.ts, jump)
        self._burst_side = "B" if direction > 0 else "S"
        self._burst_left = sim.rng.randint(self.burst_orders // 2, self.burst_orders)
        self.next_ts = sim.ts + sim.jitter(self.burst_gap_ms)
