"""The simulation loop.

Agents are scheduled on a heap keyed by their next action time, so the cost
of a run is proportional to the number of actions rather than to the length
of the simulated day. Everything is driven by one seeded `random.Random`:
the same config and the same seed produce a byte-identical tape, which is
what makes a threshold sweep comparable across runs and a regression
attributable to a code change rather than to luck.

Events are accumulated rather than streamed, because multi-symbol runs merge
several symbol tapes into one sequenced stream and sequence numbers have to
be contiguous across the merged file.
"""

from __future__ import annotations

import heapq
import random
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable

from tapewatch import tape
from tapewatch.model import Episode

from .agents import Agent, InformedTrader, MarketMaker, MomentumFollower, NoiseTrader, Taker
from .book import BUY, SELL, Fill, MatchingBook
from .manipulators import Igniter, Layerer, Spoofer, WashTrader

MS = 1_000_000
SEC = 1_000_000_000


@dataclass
class SimResult:
    events: list[tape.Event]
    episodes: list[Episode]
    roster: dict[int, str]
    information_events: list[dict[str, Any]]
    stats: dict[str, Any]


class Simulator:
    def __init__(self, symbol: str, cfg: dict, rng: random.Random, pid_base: int,
                 episode_counter: Callable[[], int]) -> None:
        self.symbol = symbol
        self.cfg = cfg
        self.rng = rng
        self.book = MatchingBook()
        self.ts = 0
        self.fair = int(cfg["market"]["reference_price"])
        self.tick = int(cfg["market"].get("tick_size", 1))
        self.events: list[tape.Event] = []
        self.episodes: list[Episode] = []
        self.roster: dict[int, str] = {}
        self.information_events: list[dict[str, Any]] = []
        self.agents: dict[int, Agent] = {}
        self._heap: list[tuple[int, int, str, Any]] = []
        self._counter = 0
        self._mids: deque[tuple[int, float]] = deque(maxlen=8192)
        self._next_episode_id = episode_counter
        self._pid_base = pid_base
        self._post_only_slides = 0
        self._build_participants()

    # -- participant construction ---------------------------------------

    def _pid(self) -> int:
        self._pid_base += 1
        return self._pid_base

    def _add(self, agent: Agent, role: str) -> Agent:
        agent.role = role
        self.agents[agent.pid] = agent
        self.roster[agent.pid] = role
        return agent

    def _build_participants(self) -> None:
        p = self.cfg["participants"]
        m = self.cfg["market"]

        for _ in range(int(p["market_makers"])):
            self._add(
                MarketMaker(
                    pid=self._pid(),
                    role="market_maker",
                    levels=int(m.get("mm_levels", 3)),
                    base_size=int(m.get("mm_size", 30)),
                    half_spread=int(m.get("mm_half_spread", 2)),
                    interval_ms=int(m.get("mm_interval_ms", 120)),
                    reprice_ticks=int(m.get("mm_reprice_ticks", 1)),
                    skew_per_lot=float(m.get("mm_skew_per_lot", 0.02)),
                    max_inventory=int(m.get("mm_max_inventory", 400)),
                    flatten_at=int(m.get("mm_flatten_at", 220)),
                    flatten_clip=int(m.get("mm_flatten_clip", 90)),
                ),
                "market_maker",
            )
        for _ in range(int(p["noise_traders"])):
            self._add(NoiseTrader(pid=self._pid(), role="noise"), "noise")
        for _ in range(int(p["takers"])):
            self._add(Taker(pid=self._pid(), role="taker"), "taker")
        for _ in range(int(p["momentum_followers"])):
            self._add(MomentumFollower(pid=self._pid(), role="momentum_follower"),
                      "momentum_follower")
        for _ in range(int(p["informed_traders"])):
            self._add(
                InformedTrader(
                    pid=self._pid(),
                    role="informed",
                    interval_ms=int(m.get("information_interval_ms", 40000)),
                ),
                "informed",
            )

        ab = self.cfg["abuse"]
        for _ in range(int(ab["spoofers"])):
            self._add(
                Spoofer(pid=self._pid(), role="spoofer",
                        interval_ms=int(ab.get("spoof_interval_ms", 45000))),
                "spoofer",
            )
        for _ in range(int(ab["layerers"])):
            self._add(
                Layerer(pid=self._pid(), role="layerer",
                        interval_ms=int(ab.get("layer_interval_ms", 60000))),
                "layerer",
            )
        for _ in range(int(ab["wash_pairs"])):
            partner = self._pid()
            main = self._pid()
            self.roster[partner] = "wash_counterparty"
            self._add(
                WashTrader(pid=main, role="wash_trader", counterparty=partner,
                           interval_ms=int(ab.get("wash_interval_ms", 70000))),
                "wash_trader",
            )
        for _ in range(int(ab["igniters"])):
            self._add(
                Igniter(pid=self._pid(), role="igniter",
                        interval_ms=int(ab.get("ignite_interval_ms", 75000)),
                        size=int(ab.get("ignite_size", 90)),
                        burst_orders=int(ab.get("ignite_orders", 8))),
                "igniter",
            )

    # -- helpers used by agents ------------------------------------------

    def jitter(self, ms: int) -> int:
        """A delay of about `ms`, never zero, never negative."""
        return max(1 * MS, int(ms * MS * self.rng.uniform(0.6, 1.4)))

    def advance_micro(self, ns: int) -> None:
        self.ts += max(1, ns)

    def next_episode_id(self) -> int:
        return self._next_episode_id()

    def record_episode(self, ep: Episode) -> None:
        self.episodes.append(ep)

    def note_information_event(self, pid: int, ts: int, jump: int) -> None:
        self.information_events.append({"participant": pid, "ts": ts, "jump_ticks": jump})

    def mid_move(self, lookback_ms: int) -> float | None:
        if not self._mids:
            return None
        now_mid = self._mids[-1][1]
        cutoff = self.ts - lookback_ms * MS
        past = None
        for ts, mid in reversed(self._mids):
            if ts <= cutoff:
                past = mid
                break
        if past is None:
            past = self._mids[0][1]
        return (now_mid - past) / self.tick

    # -- event emission ---------------------------------------------------

    def _emit(self, ev: tape.Event) -> None:
        ev.ts = self.ts
        ev.symbol = self.symbol
        self.events.append(ev)

    def _sample_mid(self) -> None:
        mid = self.book.mid()
        if mid is None:
            return
        if not self._mids or self._mids[-1][1] != mid:
            self._mids.append((self.ts, mid))

    def _emit_fills(self, taker_pid: int, taker_oid: int, side: str, fills: list[Fill]) -> None:
        for f in fills:
            self._emit(
                tape.Event(
                    kind=tape.TRADE,
                    ts=self.ts,
                    seq=0,
                    eid=0,
                    symbol=self.symbol,
                    participant=taker_pid,
                    order_id=taker_oid,
                    maker_participant=f.maker_pid,
                    maker_order_id=f.maker_oid,
                    side=side,
                    price=f.price,
                    quantity=f.qty,
                )
            )
            maker = self.agents.get(f.maker_pid)
            on_fill = getattr(maker, "on_fill", None)
            if on_fill is not None:
                # The maker's own direction is the opposite of the aggressor's.
                on_fill(SELL if side == BUY else BUY, f.qty)
            taker = self.agents.get(taker_pid)
            taker_on_fill = getattr(taker, "on_fill", None)
            if taker_on_fill is not None:
                taker_on_fill(side, f.qty)

    def _slide_post_only(self, side: str, price: int) -> int:
        """Keep a passive order passive.

        A post-only order that would cross is slid to the far side of the
        touch rather than rejected. Rejecting would be equally defensible;
        sliding keeps the participant in the book, which is what makes the
        market-maker confounder behave like one.
        """
        opposite = SELL if side == BUY else BUY
        touch = self.book.best(opposite)
        if touch is None:
            return price
        if side == BUY and price >= touch:
            self._post_only_slides += 1
            return touch - self.tick
        if side == SELL and price <= touch:
            self._post_only_slides += 1
            return touch + self.tick
        return price

    def place(self, pid: int, side: str, price: int, qty: int,
              post_only: bool = False) -> int | None:
        if qty <= 0:
            return None
        if post_only:
            price = self._slide_post_only(side, price)
        oid, fills, resting = self.book.submit(self.ts, pid, side, price, qty,
                                               post_only=post_only)
        if fills:
            self._emit_fills(pid, oid, side, fills)
        if resting > 0:
            self._emit(
                tape.Event(
                    kind=tape.ORDER,
                    ts=self.ts,
                    seq=0,
                    eid=0,
                    symbol=self.symbol,
                    participant=pid,
                    order_id=oid,
                    side=side,
                    price=price,
                    quantity=resting,
                )
            )
        self._sample_mid()
        return oid if resting > 0 else None

    def limit_take(self, pid: int, side: str, price: int, qty: int) -> list[Fill]:
        """A marketable limit order at an exact price. Never rests."""
        oid = self.book.next_oid()
        _, fills, resting = self.book.submit(self.ts, pid, side, price, qty, oid=oid)
        if fills:
            self._emit_fills(pid, oid, side, fills)
        if resting > 0:
            # The remainder would rest; withdraw it immediately so the pattern
            # under test is exactly the prints and nothing else.
            self.book.cancel(oid)
        self._sample_mid()
        return fills

    def market(self, pid: int, side: str, qty: int) -> list[Fill]:
        if qty <= 0:
            return []
        oid = self.book.next_oid()
        _, fills, _ = self.book.submit(self.ts, pid, side, None, qty, oid=oid)
        if fills:
            self._emit_fills(pid, oid, side, fills)
        self._sample_mid()
        return fills

    def cancel(self, pid: int, oid: int) -> bool:
        order = self.book.cancel(oid)
        if order is None:
            return False
        self._emit(
            tape.Event(
                kind=tape.CANCEL,
                ts=self.ts,
                seq=0,
                eid=0,
                symbol=self.symbol,
                participant=pid,
                order_id=oid,
            )
        )
        self._sample_mid()
        return True

    def amend(self, pid: int, oid: int, new_qty: int) -> bool:
        order = self.book.orders.get(oid)
        if order is None:
            return False
        self.book.reduce(oid, new_qty)
        self._emit(
            tape.Event(
                kind=tape.MODIFY,
                ts=self.ts,
                seq=0,
                eid=0,
                symbol=self.symbol,
                participant=pid,
                order_id=oid,
                side=order.side,
                price=order.price,
                quantity=max(0, new_qty),
            )
        )
        self._sample_mid()
        return True

    def schedule_cancel(self, pid: int, oid: int, ts: int) -> None:
        self._push(ts, "cancel", (pid, oid))

    # -- the loop ---------------------------------------------------------

    def _push(self, ts: int, kind: str, payload: Any) -> None:
        self._counter += 1
        heapq.heappush(self._heap, (ts, self._counter, kind, payload))

    def run(self, duration_ns: int) -> SimResult:
        warmup = int(self.cfg["market"].get("warmup_ms", 2000)) * MS
        fv_interval = int(self.cfg["market"].get("fair_value_interval_ms", 250)) * MS
        fv_sigma = float(self.cfg["market"].get("fair_value_sigma_ticks", 0.6))

        # Market makers go first so there is a book before anybody can trade
        # into it. Everyone else starts after the warm-up.
        for agent in self.agents.values():
            start = 0 if isinstance(agent, MarketMaker) else warmup
            self._push(start + self.rng.randrange(1, 5000) * MS // 5, "agent", agent)
        self._push(fv_interval, "fair", None)

        while self._heap:
            ts, _, kind, payload = heapq.heappop(self._heap)
            if ts > duration_ns:
                break
            # Never let the clock run backwards: an agent's action can advance
            # it past another agent's scheduled time.
            self.ts = max(self.ts, ts)

            if kind == "fair":
                self.fair += int(round(self.rng.gauss(0.0, fv_sigma)))
                self._push(self.ts + fv_interval, "fair", None)
                continue
            if kind == "cancel":
                pid, oid = payload
                self.cancel(pid, oid)
                continue

            agent: Agent = payload
            agent.next_ts = self.ts
            agent.act(self)
            if agent.next_ts <= self.ts:
                agent.next_ts = self.ts + 1 * MS
            self._push(agent.next_ts, "agent", agent)

        self.events.sort(key=lambda e: e.ts)
        stats = {
            "post_only_slides": self._post_only_slides,
            "resting_at_end": self.book.resting_count(),
            "fair_value_end": self.fair,
        }
        return SimResult(self.events, self.episodes, self.roster, self.information_events, stats)
