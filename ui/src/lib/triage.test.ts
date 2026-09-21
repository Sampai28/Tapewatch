import { describe, expect, it } from "vitest";

import { alertRow } from "../test/fixtures";
import { applyQueue, emptyFilter, groupIntoCases, matchesFilter, sortAlerts } from "./triage";

const SEC = 1_000_000_000;

describe("matchesFilter", () => {
  it("hides alerts below the operating point by default", () => {
    const f = emptyFilter();
    expect(f.onlyTuned).toBe(true);
    expect(matchesFilter(alertRow({ above_operating_point: false }), f)).toBe(false);
    expect(matchesFilter(alertRow({ above_operating_point: true }), f)).toBe(true);
  });

  it("matches a participant on either side of a wash pair", () => {
    const f = { ...emptyFilter(), participant: 1037 };
    expect(matchesFilter(alertRow({ participant: 1037 }), f)).toBe(true);
    expect(matchesFilter(alertRow({ participant: 1036, counterparty: 1037 }), f)).toBe(true);
    expect(matchesFilter(alertRow({ participant: 1036, counterparty: 1035 }), f)).toBe(false);
  });

  it("searches ids, detector, symbol and role", () => {
    const f = { ...emptyFilter(), search: "wash" };
    expect(matchesFilter(alertRow({ detector: "wash_trading" }), f)).toBe(true);
    expect(matchesFilter(alertRow({ role: "wash_counterparty" }), f)).toBe(true);
    expect(matchesFilter(alertRow({ detector: "spoofing", role: "spoofer" }), f)).toBe(false);
  });

  it("can hide alerts raised over a damaged feed", () => {
    const f = { ...emptyFilter(), hideDegraded: true };
    expect(matchesFilter(alertRow({ degraded: true }), f)).toBe(false);
    expect(matchesFilter(alertRow({ degraded: false }), f)).toBe(true);
  });

  it("treats an empty detector set as no detector filter, not as a dead queue", () => {
    // Unticking every chip should not silently empty the queue with no
    // explanation on screen.
    const f = { ...emptyFilter(), detectors: new Set<never>() };
    expect(matchesFilter(alertRow(), f)).toBe(true);
  });
});

describe("sortAlerts", () => {
  const a = alertRow({ alert_id: 1, score: 0.9, confidence: 0.54, detect_ts: 30 * SEC });
  const b = alertRow({ alert_id: 2, score: 0.6, confidence: 0.6, detect_ts: 10 * SEC });

  it("ranks by confidence by default, not raw score", () => {
    // The degraded alert scores higher and is trusted less; the queue must
    // put the trustworthy one first or the discount means nothing.
    expect(sortAlerts([a, b], "confidence").map((x) => x.alert_id)).toEqual([2, 1]);
    expect(sortAlerts([a, b], "score").map((x) => x.alert_id)).toEqual([1, 2]);
  });

  it("orders by time when asked", () => {
    expect(sortAlerts([a, b], "oldest").map((x) => x.alert_id)).toEqual([2, 1]);
    expect(sortAlerts([a, b], "recent").map((x) => x.alert_id)).toEqual([1, 2]);
  });

  it("breaks ties deterministically", () => {
    const x = alertRow({ alert_id: 7, confidence: 0.5 });
    const y = alertRow({ alert_id: 3, confidence: 0.5 });
    expect(sortAlerts([x, y], "confidence").map((v) => v.alert_id)).toEqual([3, 7]);
  });

  it("does not mutate its input", () => {
    const input = [a, b];
    sortAlerts(input, "confidence");
    expect(input.map((x) => x.alert_id)).toEqual([1, 2]);
  });
});

describe("applyQueue", () => {
  it("filters and sorts in one pass", () => {
    const rows = [
      alertRow({ alert_id: 1, detector: "spoofing", confidence: 0.4 }),
      alertRow({ alert_id: 2, detector: "layering", confidence: 0.9 }),
      alertRow({ alert_id: 3, detector: "spoofing", confidence: 0.8 }),
    ];
    const f = { ...emptyFilter(), detectors: new Set(["spoofing" as const]) };
    expect(applyQueue(rows, f).map((r) => r.alert_id)).toEqual([3, 1]);
  });
});

describe("groupIntoCases", () => {
  it("groups one participant's overlapping alerts into a single case", () => {
    const rows = [
      alertRow({ alert_id: 1, participant: 50, start_ts: 0, end_ts: 2 * SEC, confidence: 0.6 }),
      alertRow({ alert_id: 2, participant: 50, start_ts: 3 * SEC, end_ts: 4 * SEC,
                 confidence: 0.9 }),
      alertRow({ alert_id: 3, participant: 50, start_ts: 90 * SEC, end_ts: 91 * SEC,
                 confidence: 0.5 }),
      alertRow({ alert_id: 4, participant: 51, start_ts: 1 * SEC, end_ts: 2 * SEC,
                 confidence: 0.7 }),
    ];
    const groups = groupIntoCases(rows, 10 * SEC);
    expect(groups).toHaveLength(3);
    // Ranked by the strongest alert in each group.
    expect(groups[0]?.map((a) => a.alert_id)).toEqual([1, 2]);
    expect(groups[1]?.map((a) => a.alert_id)).toEqual([4]);
    expect(groups[2]?.map((a) => a.alert_id)).toEqual([3]);
  });

  it("never merges different participants", () => {
    const rows = [
      alertRow({ alert_id: 1, participant: 1, start_ts: 0, end_ts: SEC }),
      alertRow({ alert_id: 2, participant: 2, start_ts: 0, end_ts: SEC }),
    ];
    expect(groupIntoCases(rows, 60 * SEC)).toHaveLength(2);
  });
});
