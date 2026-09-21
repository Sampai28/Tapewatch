"""Deciding whether an alert caught an episode.

This is the single most consequential file in the evaluation, because every
headline number is downstream of it and a generous rule here makes a bad
detector look good. The rule is stated in full rather than left implicit:

An alert MATCHES an episode when all three hold:

  1. same symbol;
  2. the participants intersect. For most detectors that means the alert
     names the episode's participant. For wash trading both sides are named
     by both, and naming either one is a hit -- the pair is the pattern, and
     which of the two accounts gets listed first is an artefact of sorting;
  3. the alert's window [start, end] overlaps the episode's window widened
     by a tolerance: [start - pre, end + post].

The tolerance is asymmetric and that is deliberate. `pre` is small: an alert
that fires *before* the behaviour it describes is a coincidence, not a
detection. `post` is large, because two of the four detectors cannot
conclude anything until after the fact -- momentum ignition has to wait for
a reversal that has not happened yet -- and a rule that punished them for it
would be measuring the definition of the pattern rather than the quality of
the detector.

A TYPED match additionally requires alert.detector == episode.kind. Typed
matches drive the headline precision and recall. Untyped matches are counted
separately as `misclassified`, and they are not thrown in with the false
positives: an alert that correctly identifies abuse and calls it by the
wrong name has done most of an analyst's job, and burying it in the FP
column would understate the system while overstating its own precision
elsewhere. Both numbers are reported.

Duplicates -- several alerts on one episode -- are counted but are neither
true positives nor false positives. They cost analyst time, which is what
the alert-volume metrics are for, and counting them as TPs would let a
detector buy precision by firing repeatedly on the cases it already found.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable

from tapewatch.model import Alert, Episode


@dataclass
class MatchResult:
    # episode_id -> alerts that matched it with the right type, in detect order
    episode_hits: dict[int, list[Alert]] = field(default_factory=dict)
    # episode_id -> alerts that matched it but called it something else
    episode_offtype: dict[int, list[Alert]] = field(default_factory=dict)
    # alert_id -> episode_id it was credited to (typed)
    alert_to_episode: dict[int, int] = field(default_factory=dict)
    # alert_id -> episode_id it overlapped with the wrong detector
    alert_misclassified: dict[int, int] = field(default_factory=dict)
    false_positives: list[Alert] = field(default_factory=list)
    duplicates: list[Alert] = field(default_factory=list)
    episodes: list[Episode] = field(default_factory=list)
    alerts: list[Alert] = field(default_factory=list)

    def detected(self) -> list[Episode]:
        return [e for e in self.episodes if self.episode_hits.get(e.episode_id)]

    def missed(self) -> list[Episode]:
        return [e for e in self.episodes if not self.episode_hits.get(e.episode_id)]

    def first_hit(self, episode_id: int) -> Alert | None:
        hits = self.episode_hits.get(episode_id)
        return hits[0] if hits else None


def _participants(obj) -> set[int]:
    out = {obj.participant}
    cp = getattr(obj, "counterparty", None)
    if cp is not None:
        out.add(cp)
    return out


def overlaps(alert: Alert, episode: Episode, pre_ns: int, post_ns: int) -> bool:
    lo = episode.start_ts - pre_ns
    hi = episode.end_ts + post_ns
    return alert.start_ts <= hi and alert.end_ts >= lo


def match(
    alerts: Iterable[Alert],
    episodes: Iterable[Episode],
    pre_tolerance_ns: int,
    post_tolerance_ns: int,
) -> MatchResult:
    eps = list(episodes)
    als = sorted(alerts, key=lambda a: (a.detect_ts, a.alert_id))
    result = MatchResult(episodes=eps, alerts=als)

    # Bucket episodes by symbol so the scan below is over a candidate list
    # rather than everything. Episode counts are small; this only matters for
    # configs/full.yaml.
    by_symbol: dict[str, list[Episode]] = {}
    for ep in eps:
        by_symbol.setdefault(ep.symbol, []).append(ep)
    for group in by_symbol.values():
        group.sort(key=lambda e: e.start_ts)

    for ep in eps:
        result.episode_hits.setdefault(ep.episode_id, [])
        result.episode_offtype.setdefault(ep.episode_id, [])

    for alert in als:
        typed: Episode | None = None
        untyped: Episode | None = None
        for ep in by_symbol.get(alert.symbol, ()):
            if not (_participants(alert) & _participants(ep)):
                continue
            if not overlaps(alert, ep, pre_tolerance_ns, post_tolerance_ns):
                continue
            if alert.detector == ep.kind:
                # Prefer the episode that starts first, so a detector cannot
                # gain by being credited to whichever episode flatters it.
                if typed is None or ep.start_ts < typed.start_ts:
                    typed = ep
            elif untyped is None or ep.start_ts < untyped.start_ts:
                untyped = ep

        if typed is not None:
            already = result.episode_hits[typed.episode_id]
            already.append(alert)
            result.alert_to_episode[alert.alert_id] = typed.episode_id
            if len(already) > 1:
                result.duplicates.append(alert)
        elif untyped is not None:
            result.episode_offtype[untyped.episode_id].append(alert)
            result.alert_misclassified[alert.alert_id] = untyped.episode_id
        else:
            result.false_positives.append(alert)

    return result
