// Queue logic, kept out of the components so it can be tested without a DOM.
//
// The ranking question this file answers is the whole job of a triage queue:
// given several hundred alerts and one analyst, what should they look at
// first? Sorting by raw score is wrong, because an alert raised over a
// damaged feed is less trustworthy at the same score. Sorting by confidence
// -- score discounted for feed damage -- is the default here for that reason.

import type { AlertRow, Detector, Outcome } from "../types";

export type SortKey = "confidence" | "score" | "recent" | "oldest";

export interface QueueFilter {
  detectors: Set<Detector>;
  status: "all" | "open" | "reviewing" | "closed";
  onlyTuned: boolean;
  hideDegraded: boolean;
  participant: number | null;
  search: string;
  sort: SortKey;
}

export const ALL_DETECTORS: Detector[] = [
  "spoofing",
  "layering",
  "wash_trading",
  "momentum_ignition",
];

export function emptyFilter(): QueueFilter {
  return {
    detectors: new Set(ALL_DETECTORS),
    status: "all",
    onlyTuned: true,
    hideDegraded: false,
    participant: null,
    search: "",
    sort: "confidence",
  };
}

export function matchesFilter(alert: AlertRow, f: QueueFilter): boolean {
  if (f.detectors.size > 0 && !f.detectors.has(alert.detector)) return false;
  if (f.status !== "all" && alert.status !== f.status) return false;
  if (f.onlyTuned && !alert.above_operating_point) return false;
  if (f.hideDegraded && alert.degraded) return false;
  if (f.participant !== null &&
      alert.participant !== f.participant &&
      alert.counterparty !== f.participant) {
    return false;
  }
  const needle = f.search.trim().toLowerCase();
  if (needle) {
    const hay = [
      String(alert.alert_id),
      alert.detector,
      alert.symbol,
      String(alert.participant),
      alert.counterparty === null ? "" : String(alert.counterparty),
      alert.role,
    ]
      .join(" ")
      .toLowerCase();
    if (!hay.includes(needle)) return false;
  }
  return true;
}

export function sortAlerts(alerts: AlertRow[], sort: SortKey): AlertRow[] {
  const out = [...alerts];
  switch (sort) {
    case "score":
      out.sort((a, b) => b.score - a.score || a.alert_id - b.alert_id);
      break;
    case "recent":
      out.sort((a, b) => b.detect_ts - a.detect_ts || a.alert_id - b.alert_id);
      break;
    case "oldest":
      out.sort((a, b) => a.detect_ts - b.detect_ts || a.alert_id - b.alert_id);
      break;
    case "confidence":
    default:
      out.sort((a, b) => b.confidence - a.confidence || a.alert_id - b.alert_id);
      break;
  }
  return out;
}

export function applyQueue(alerts: AlertRow[], f: QueueFilter): AlertRow[] {
  return sortAlerts(alerts.filter((a) => matchesFilter(a, f)), f.sort);
}

/**
 * Alerts that name the same participant and overlap in time are one
 * investigation, not several. Grouping them is what turns "four hundred
 * alerts" into a number of cases a desk can actually work.
 */
export function groupIntoCases(alerts: AlertRow[], gapNs: number): AlertRow[][] {
  const byParticipant = new Map<number, AlertRow[]>();
  for (const a of alerts) {
    const list = byParticipant.get(a.participant);
    if (list) list.push(a);
    else byParticipant.set(a.participant, [a]);
  }
  const groups: AlertRow[][] = [];
  for (const list of byParticipant.values()) {
    list.sort((x, y) => x.start_ts - y.start_ts);
    let current: AlertRow[] = [];
    let end = -Infinity;
    for (const a of list) {
      if (current.length > 0 && a.start_ts - end > gapNs) {
        groups.push(current);
        current = [];
      }
      current.push(a);
      end = Math.max(end, a.end_ts);
    }
    if (current.length > 0) groups.push(current);
  }
  groups.sort(
    (x, y) =>
      Math.max(...y.map((a) => a.confidence)) - Math.max(...x.map((a) => a.confidence)),
  );
  return groups;
}

export function outcomeLabel(outcome: Outcome): string {
  switch (outcome) {
    case "true_positive":
      return "matched an injected episode";
    case "misclassified":
      return "overlapped an episode of another kind";
    case "duplicate":
      return "repeat alert on an episode already found";
    case "false_positive":
    default:
      return "no injected episode";
  }
}

export function counts(alerts: AlertRow[]): Record<Outcome, number> {
  const out: Record<Outcome, number> = {
    true_positive: 0,
    false_positive: 0,
    misclassified: 0,
    duplicate: 0,
  };
  for (const a of alerts) out[a.outcome] += 1;
  return out;
}
