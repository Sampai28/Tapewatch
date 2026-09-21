import { describe, expect, it } from "vitest";

import { frame } from "../test/fixtures";
import { describeFrame, frameIndexAt, ladderRows, ladderScale } from "./ladder";

const SEC = 1_000_000_000;

describe("ladderScale", () => {
  it("collects every price the replay ever touched", () => {
    const frames = [
      frame({ bids: [{ price: 100, quantity: 10, orders: 1, subject: false, subject_qty: 0 }],
              asks: [] }),
      frame({ bids: [{ price: 99, quantity: 40, orders: 1, subject: false, subject_qty: 0 }],
              asks: [{ price: 102, quantity: 5, orders: 1, subject: false, subject_qty: 0 }] }),
    ];
    const scale = ladderScale(frames);
    expect(scale.prices).toEqual([102, 100, 99]);
    expect(scale.maxQty).toBe(40);
  });

  it("keeps the row set stable across frames", () => {
    // The whole point: a price that is empty in this frame still gets a row,
    // so the ladder does not jump when a level disappears -- which is
    // precisely the moment a spoofing alert is about.
    const frames = [
      frame({ bids: [{ price: 100, quantity: 240, orders: 1, subject: true, subject_qty: 240 }],
              asks: [] }),
      frame({ bids: [], asks: [] }),
    ];
    const scale = ladderScale(frames);
    const before = ladderRows(frames[0], scale);
    const after = ladderRows(frames[1], scale);
    expect(before.map((r) => r.price)).toEqual(after.map((r) => r.price));
    expect(before[0]?.bidQty).toBe(240);
    expect(after[0]?.bidQty).toBe(0);
  });

  it("caps the row count around the middle of the range", () => {
    const frames = [
      frame({
        bids: Array.from({ length: 60 }, (_, i) => ({
          price: 1000 - i, quantity: 1, orders: 1, subject: false, subject_qty: 0,
        })),
        asks: [],
      }),
    ];
    const scale = ladderScale(frames, 10);
    expect(scale.prices).toHaveLength(10);
    expect(scale.prices[0]).toBeLessThan(1000);
    expect(scale.prices[0]).toBeGreaterThan(941);
  });

  it("survives an empty replay", () => {
    const scale = ladderScale([]);
    expect(scale.prices).toEqual([]);
    expect(ladderRows(undefined, scale)).toEqual([]);
  });
});

describe("ladderRows", () => {
  it("marks the alert's own depth on the correct side", () => {
    const f = frame({
      bids: [{ price: 100, quantity: 240, orders: 1, subject: true, subject_qty: 240 }],
      asks: [{ price: 104, quantity: 30, orders: 1, subject: false, subject_qty: 0 }],
    });
    const rows = ladderRows(f, ladderScale([f]));
    const bid = rows.find((r) => r.price === 100);
    const ask = rows.find((r) => r.price === 104);
    expect(bid?.bidSubject).toBe(true);
    expect(bid?.askSubject).toBe(false);
    expect(ask?.askSubject).toBe(false);
    expect(ask?.askQty).toBe(30);
  });
});

describe("frameIndexAt", () => {
  const frames = [0, 1, 2, 5, 9].map((s) => frame({ ts: s * SEC }));

  it("finds the last frame at or before a timestamp", () => {
    expect(frameIndexAt(frames, 0)).toBe(0);
    expect(frameIndexAt(frames, 2 * SEC)).toBe(2);
    expect(frameIndexAt(frames, 4 * SEC)).toBe(2);
    expect(frameIndexAt(frames, 100 * SEC)).toBe(4);
  });

  it("clamps before the first frame rather than returning -1", () => {
    expect(frameIndexAt(frames, -5)).toBe(0);
    expect(frameIndexAt([], 10)).toBe(0);
  });
});

describe("describeFrame", () => {
  it("names each record kind in market terms", () => {
    expect(describeFrame(frame({ kind: "O", side: "B", quantity: 240 }))).toContain("bid 240");
    expect(describeFrame(frame({ kind: "X", order_id: 77 }))).toBe("cancel 77");
    expect(describeFrame(frame({ kind: "M", order_id: 77, quantity: 5 }))).toBe(
      "amend 77 to 5",
    );
    expect(describeFrame(frame({ kind: "T", side: "S", quantity: 20 }))).toContain(
      "seller aggressed",
    );
  });
});
