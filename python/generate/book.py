"""A price-time-priority matching book for the generator.

The tape has to be internally consistent: a trade must reference a resting
order that existed, at a price that was on the book, for a quantity that was
available. Generating events from a statistical model without matching them
produces a file that looks like market data and falls apart the moment the
engine reconstructs a book from it -- unknown makers, negative depth, crossed
quotes everywhere -- and then every feed-integrity counter measures the
generator instead of the feed.

So the generator runs a real, if small, matching engine. It is not fast and it
does not need to be; it is price-time priority, integer prices, FIFO at each
level, and nothing else.

Self-trading is permitted. A venue with self-trade prevention would reject it,
and then direct wash trading would be invisible on the tape rather than
detectable, so the modelled venue does not have it. That is a modelling
choice and it is stated in docs/notes.md rather than buried here.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

BUY = "B"
SELL = "S"


@dataclass
class Order:
    oid: int
    pid: int
    side: str
    price: int
    qty: int
    ts: int


@dataclass
class Fill:
    maker_pid: int
    maker_oid: int
    price: int
    qty: int
    maker_exhausted: bool


class MatchingBook:
    def __init__(self) -> None:
        self.levels: dict[str, dict[int, list[Order]]] = {BUY: {}, SELL: {}}
        self.orders: dict[int, Order] = {}
        self._next_oid = 1

    # -- queries ---------------------------------------------------------

    def best(self, side: str) -> int | None:
        prices = self.levels[side]
        if not prices:
            return None
        return max(prices) if side == BUY else min(prices)

    def best_bid(self) -> int | None:
        return self.best(BUY)

    def best_ask(self) -> int | None:
        return self.best(SELL)

    def mid(self) -> float | None:
        bb, ba = self.best_bid(), self.best_ask()
        if bb is None or ba is None:
            return None
        return (bb + ba) / 2.0

    def depth(self, side: str, price: int) -> int:
        return sum(o.qty for o in self.levels[side].get(price, ()))

    def side_qty(self, side: str) -> int:
        return sum(o.qty for orders in self.levels[side].values() for o in orders)

    def resting_count(self) -> int:
        return len(self.orders)

    def participant_orders(self, pid: int, side: str | None = None) -> list[Order]:
        return [
            o
            for o in self.orders.values()
            if o.pid == pid and (side is None or o.side == side)
        ]

    # -- mutation --------------------------------------------------------

    def next_oid(self) -> int:
        oid = self._next_oid
        self._next_oid += 1
        return oid

    def submit(
        self,
        ts: int,
        pid: int,
        side: str,
        price: int | None,
        qty: int,
        oid: int | None = None,
        post_only: bool = False,
    ) -> tuple[int, list[Fill], int]:
        """Submit an order.

        `price=None` means a market order: it takes whatever is there and
        never rests. `post_only` refuses to cross and rests at `price`
        regardless, which is how the market makers stay passive.

        Returns (order id, fills in order, quantity left resting).
        """
        if oid is None:
            oid = self.next_oid()
        fills: list[Fill] = []
        remaining = qty
        opposite = SELL if side == BUY else BUY

        if not post_only:
            while remaining > 0:
                touch = self.best(opposite)
                if touch is None:
                    break
                if price is not None:
                    if side == BUY and touch > price:
                        break
                    if side == SELL and touch < price:
                        break
                queue = self.levels[opposite][touch]
                while remaining > 0 and queue:
                    maker = queue[0]
                    traded = min(remaining, maker.qty)
                    maker.qty -= traded
                    remaining -= traded
                    exhausted = maker.qty == 0
                    fills.append(Fill(maker.pid, maker.oid, touch, traded, exhausted))
                    if exhausted:
                        queue.pop(0)
                        self.orders.pop(maker.oid, None)
                if not queue:
                    del self.levels[opposite][touch]

        if remaining > 0 and price is not None:
            order = Order(oid, pid, side, price, remaining, ts)
            self.levels[side].setdefault(price, []).append(order)
            self.orders[oid] = order
        else:
            remaining = 0 if price is None else remaining

        return oid, fills, (remaining if price is not None else 0)

    def cancel(self, oid: int) -> Order | None:
        order = self.orders.pop(oid, None)
        if order is None:
            return None
        queue = self.levels[order.side].get(order.price)
        if queue is not None:
            for i, o in enumerate(queue):
                if o.oid == oid:
                    queue.pop(i)
                    break
            if not queue:
                del self.levels[order.side][order.price]
        return order

    def reduce(self, oid: int, new_qty: int) -> Order | None:
        """Amend quantity in place, keeping time priority. Never increases."""
        order = self.orders.get(oid)
        if order is None or new_qty >= order.qty:
            return None
        if new_qty <= 0:
            return self.cancel(oid)
        order.qty = new_qty
        return order

    def all_orders(self) -> Iterable[Order]:
        return list(self.orders.values())
