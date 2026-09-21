import { describe, expect, it } from "vitest";

import {
  formatDurationNs,
  formatEvidence,
  formatPrice,
  formatTapeTime,
} from "./format";

describe("formatPrice", () => {
  it("renders minor units without floating point", () => {
    expect(formatPrice(10000)).toBe("100.00");
    expect(formatPrice(10005)).toBe("100.05");
    expect(formatPrice(5)).toBe("0.05");
    expect(formatPrice(0)).toBe("0.00");
  });

  it("keeps the sign on negative prices", () => {
    // Negative prices are legal in some markets and -0.05 must not render
    // as 0.-5 or 0.05.
    expect(formatPrice(-5)).toBe("-0.05");
    expect(formatPrice(-10005)).toBe("-100.05");
  });

  it("does not lose a cent to binary rounding", () => {
    // 1.15 and 2.675 are the classic float display failures. Integer minor
    // units make them exact, and this test exists so nobody "simplifies"
    // this function into a division.
    expect(formatPrice(115)).toBe("1.15");
    expect(formatPrice(2675)).toBe("26.75");
    expect(formatPrice(999999999)).toBe("9999999.99");
  });
});

describe("formatTapeTime", () => {
  it("shows a position within the session, not an absolute clock", () => {
    const origin = 1_000_000_000;
    expect(formatTapeTime(origin, origin)).toBe("00:00:00.000");
    expect(formatTapeTime(origin + 1_500_000_000, origin)).toBe("00:00:01.500");
    expect(formatTapeTime(origin + 3_661_000_000_000, origin)).toBe("01:01:01.000");
  });

  it("clamps events before the origin instead of showing negative time", () => {
    expect(formatTapeTime(5, 1_000_000_000)).toBe("00:00:00.000");
  });
});

describe("formatDurationNs", () => {
  it("picks a readable unit", () => {
    expect(formatDurationNs(500)).toBe("500 ns");
    expect(formatDurationNs(2_000_000)).toBe("2 ms");
    expect(formatDurationNs(2_500_000_000)).toBe("2.50 s");
  });
});

describe("formatEvidence", () => {
  it("does not render a millisecond count as a quantity", () => {
    // A 300 ms cancel and a 300 lot order look identical without this.
    expect(formatEvidence("lifetime_ms", 300)).toBe("300 ms");
    expect(formatEvidence("cancelled_qty", 300)).toBe("300");
  });

  it("renders flags and percentages in their own units", () => {
    expect(formatEvidence("two_sided_pct", 45)).toBe("45%");
    expect(formatEvidence("spoof_side_is_buy", 1)).toBe("buy");
    expect(formatEvidence("spoof_side_is_buy", 0)).toBe("sell");
    expect(formatEvidence("is_self_trade", 1)).toBe("yes");
  });
});
