"""Manipulators, and the ground truth they record.

Each of these runs a scripted pattern and writes an Episode describing
exactly what it did and when. That record is the only thing the evaluation
harness scores against, so two properties matter more than realism:

  The episode window must bound the behaviour, not the intent. It runs from
  the first order the manipulator sent to the last trade it took, because
  that is the span a detector could possibly have seen. Padding it would
  inflate recall by making alerts easier to match.

  Intensity must be recorded, not just used. Every manipulator draws an
  intensity in [0, 1] that scales how blatant it is -- order size, how fast
  it pulls, how much of the move it takes. Recall at high intensity and
  recall at low intensity are different numbers and reporting only their
  average hides the entire question of where the detector's floor is.

Each manipulator is also given ordinary business to do between episodes, so
that a participant is not identifiable purely by having done nothing else.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from tapewatch.model import Episode

from .agents import Agent


@dataclass
class Manipulator(Agent):
    intensity_range: tuple[float, float] = (0.3, 1.0)
    interval_ms: int = 60000
    idle_size: int = 10

    def draw_intensity(self, sim) -> float:
        lo, hi = self.intensity_range
        return sim.rng.uniform(lo, hi)

    def idle(self, sim) -> None:
        """Ordinary flow between episodes, so the account is not a ghost."""
        side = "B" if sim.rng.random() < 0.5 else "S"
        ref = sim.book.best(side)
        if ref is None:
            return
        off = sim.rng.randint(1, 5)
        px = ref - off if side == "B" else ref + off
        oid = sim.place(self.pid, side, px, self.idle_size, post_only=True)
        if oid is not None:
            sim.schedule_cancel(self.pid, oid, sim.ts + sim.jitter(4000))


@dataclass
class Spoofer(Manipulator):
    """Post size on one side, pull it, take the other side."""

    base_size: int = 240
    hold_ms: int = 700
    ticks_off: int = 1
    take_size: int = 45

    _stage: int = 0
    _side: str = "B"
    _oid: int | None = None
    _episode: Episode | None = None
    _intensity: float = 1.0

    def act(self, sim) -> None:
        if self._stage == 0:
            self._start(sim)
        elif self._stage == 1:
            self._pull_and_take(sim)
        else:
            self.idle(sim)
            self._stage = 0
            self.next_ts = sim.ts + sim.jitter(self.interval_ms)

    def _start(self, sim) -> None:
        touch = sim.book.best("B") if sim.rng.random() < 0.5 else None
        self._side = "B" if touch is not None else "S"
        ref = sim.book.best(self._side)
        if ref is None:
            self.next_ts = sim.ts + sim.jitter(2000)
            return
        self._intensity = self.draw_intensity(sim)
        size = max(20, int(self.base_size * (0.35 + 0.65 * self._intensity)))
        off = self.ticks_off if self._intensity > 0.6 else self.ticks_off + 2
        px = ref - off if self._side == "B" else ref + off
        oid = sim.place(self.pid, self._side, px, size, post_only=True)
        if oid is None:
            self.next_ts = sim.ts + sim.jitter(2000)
            return
        self._oid = oid
        self._episode = Episode(
            episode_id=sim.next_episode_id(),
            kind="spoofing",
            symbol=sim.symbol,
            participant=self.pid,
            start_ts=sim.ts,
            end_ts=sim.ts,
            intensity=self._intensity,
            order_ids=[oid],
            notes={"side": self._side, "size": size, "ticks_off": off},
        )
        self._stage = 1
        hold = int(self.hold_ms * (1.6 - 0.9 * self._intensity))
        self.next_ts = sim.ts + sim.jitter(hold)

    def _pull_and_take(self, sim) -> None:
        if self._oid is not None:
            sim.cancel(self.pid, self._oid)
        take_side = "S" if self._side == "B" else "B"
        qty = max(5, int(self.take_size * (0.4 + 0.6 * self._intensity)))
        sim.market(self.pid, take_side, qty)
        if self._episode is not None:
            self._episode.end_ts = sim.ts
            self._episode.notes["take_qty"] = qty
            sim.record_episode(self._episode)
        self._episode = None
        self._oid = None
        self._stage = 2
        self.next_ts = sim.ts + sim.jitter(1500)


@dataclass
class Layerer(Manipulator):
    """A stack of orders across levels, in together and out together."""

    layers: int = 5
    base_size: int = 70
    hold_ms: int = 1200
    take_size: int = 50

    _stage: int = 0
    _side: str = "B"
    _oids: list[int] = field(default_factory=list)
    _episode: Episode | None = None
    _intensity: float = 1.0

    def act(self, sim) -> None:
        if self._stage == 0:
            self._start(sim)
        elif self._stage == 1:
            self._pull_and_take(sim)
        else:
            self.idle(sim)
            self._stage = 0
            self.next_ts = sim.ts + sim.jitter(self.interval_ms)

    def _start(self, sim) -> None:
        self._side = "B" if sim.rng.random() < 0.5 else "S"
        ref = sim.book.best(self._side)
        if ref is None:
            self.next_ts = sim.ts + sim.jitter(2000)
            return
        self._intensity = self.draw_intensity(sim)
        count = max(3, int(round(self.layers * (0.5 + 0.5 * self._intensity))))
        size = max(15, int(self.base_size * (0.4 + 0.6 * self._intensity)))
        self._oids = []
        for i in range(count):
            px = ref - i if self._side == "B" else ref + i
            oid = sim.place(self.pid, self._side, px, size, post_only=True)
            if oid is not None:
                self._oids.append(oid)
        if len(self._oids) < 3:
            self.next_ts = sim.ts + sim.jitter(2000)
            return
        self._episode = Episode(
            episode_id=sim.next_episode_id(),
            kind="layering",
            symbol=sim.symbol,
            participant=self.pid,
            start_ts=sim.ts,
            end_ts=sim.ts,
            intensity=self._intensity,
            order_ids=list(self._oids),
            notes={"side": self._side, "levels": len(self._oids), "size": size},
        )
        self._stage = 1
        hold = int(self.hold_ms * (1.5 - 0.8 * self._intensity))
        self.next_ts = sim.ts + sim.jitter(hold)

    def _pull_and_take(self, sim) -> None:
        # The cancels go out together. That coordination is the signature;
        # spreading them out is what an honest withdrawal looks like.
        for oid in self._oids:
            sim.cancel(self.pid, oid)
            sim.advance_micro(sim.rng.randint(5, 40))
        take_side = "S" if self._side == "B" else "B"
        qty = max(5, int(self.take_size * (0.4 + 0.6 * self._intensity)))
        sim.market(self.pid, take_side, qty)
        if self._episode is not None:
            self._episode.end_ts = sim.ts
            self._episode.notes["take_qty"] = qty
            sim.record_episode(self._episode)
        self._episode = None
        self._oids = []
        self._stage = 2
        self.next_ts = sim.ts + sim.jitter(1500)


@dataclass
class WashTrader(Manipulator):
    """Volume without ownership change, in both shapes the tape can show."""

    counterparty: int = 0
    rounds: int = 3
    base_size: int = 60
    gap_ms: int = 400
    self_trade_probability: float = 0.5

    def act(self, sim) -> None:
        intensity = self.draw_intensity(sim)
        size = max(10, int(self.base_size * (0.4 + 0.6 * intensity)))
        rounds = max(1, int(round(self.rounds * (0.5 + 0.5 * intensity))))
        direct = sim.rng.random() < self.self_trade_probability
        mid = sim.book.mid()
        if mid is None:
            self.next_ts = sim.ts + sim.jitter(3000)
            return
        px = int(round(mid))

        start_ts = sim.ts
        order_ids: list[int] = []
        prints = 0
        for _ in range(rounds):
            if direct:
                oid = sim.place(self.pid, "S", px, size, post_only=True)
                if oid is None:
                    break
                order_ids.append(oid)
                sim.advance_micro(sim.rng.randint(20, 120))
                fills = sim.limit_take(self.pid, "B", px, size)
                prints += len(fills)
            else:
                oid = sim.place(self.counterparty, "S", px, size, post_only=True)
                if oid is not None:
                    order_ids.append(oid)
                    sim.advance_micro(sim.rng.randint(20, 120))
                    prints += len(sim.limit_take(self.pid, "B", px, size))
                sim.advance_micro(sim.rng.randint(20, 120))
                back = sim.place(self.pid, "S", px, size, post_only=True)
                if back is not None:
                    order_ids.append(back)
                    sim.advance_micro(sim.rng.randint(20, 120))
                    prints += len(sim.limit_take(self.counterparty, "B", px, size))
            sim.advance_micro(sim.rng.randint(self.gap_ms // 2, self.gap_ms) * 1000)

        if prints > 0:
            sim.record_episode(
                Episode(
                    episode_id=sim.next_episode_id(),
                    kind="wash_trading",
                    symbol=sim.symbol,
                    participant=self.pid,
                    counterparty=self.pid if direct else self.counterparty,
                    start_ts=start_ts,
                    end_ts=sim.ts,
                    intensity=intensity,
                    order_ids=order_ids,
                    notes={"direct": direct, "prints": prints, "size": size},
                )
            )
        self.next_ts = sim.ts + sim.jitter(self.interval_ms)


@dataclass
class Igniter(Manipulator):
    """Push the price with aggressive flow, then sell into what followed."""

    # An igniter that is one of twenty people buying has not ignited
    # anything -- the pattern is *being* the flow, briefly and visibly. Sizes
    # here are large relative to the ordinary takers on purpose.
    burst_orders: int = 8
    burst_gap_ms: int = 90
    size: int = 90
    pause_ms: int = 2500
    unwind_fraction: float = 0.9

    _stage: int = 0
    _side: str = "B"
    _left: int = 0
    _taken: int = 0
    _episode: Episode | None = None
    _intensity: float = 1.0

    def act(self, sim) -> None:
        if self._stage == 0:
            self._begin(sim)
        elif self._stage == 1:
            self._push(sim)
        elif self._stage == 2:
            self._unwind(sim)
        else:
            self.idle(sim)
            self._stage = 0
            self.next_ts = sim.ts + sim.jitter(self.interval_ms)

    def _begin(self, sim) -> None:
        if sim.book.mid() is None:
            self.next_ts = sim.ts + sim.jitter(3000)
            return
        self._intensity = self.draw_intensity(sim)
        self._side = "B" if sim.rng.random() < 0.5 else "S"
        self._left = max(3, int(round(self.burst_orders * (0.4 + 0.6 * self._intensity))))
        self._taken = 0
        self._episode = Episode(
            episode_id=sim.next_episode_id(),
            kind="momentum_ignition",
            symbol=sim.symbol,
            participant=self.pid,
            start_ts=sim.ts,
            end_ts=sim.ts,
            intensity=self._intensity,
            notes={"side": self._side},
        )
        self._stage = 1
        self.next_ts = sim.ts

    def _push(self, sim) -> None:
        qty = max(5, int(self.size * (0.5 + 0.5 * self._intensity) * sim.rng.uniform(0.8, 1.2)))
        fills = sim.market(self.pid, self._side, qty)
        self._taken += sum(f.qty for f in fills)
        self._left -= 1
        if self._left > 0:
            self.next_ts = sim.ts + sim.jitter(self.burst_gap_ms)
            return
        self._stage = 2
        self.next_ts = sim.ts + sim.jitter(self.pause_ms)

    def _unwind(self, sim) -> None:
        back = "S" if self._side == "B" else "B"
        qty = max(1, int(self._taken * self.unwind_fraction))
        # Unwinding in one clip would be unrealistic and would also make the
        # reversal trivially easy to spot; split it the way a real one is.
        chunks = 3
        for _ in range(chunks):
            sim.market(self.pid, back, max(1, qty // chunks))
            sim.advance_micro(sim.rng.randint(80, 250) * 1000)
        if self._episode is not None:
            self._episode.end_ts = sim.ts
            self._episode.notes["burst_qty"] = self._taken
            self._episode.notes["unwind_qty"] = qty
            sim.record_episode(self._episode)
        self._episode = None
        self._stage = 3
        self.next_ts = sim.ts + sim.jitter(2000)
