// Display helpers.
//
// Prices arrive as integer minor units and are never converted to a float for
// arithmetic -- only for display, at the last possible moment. Timestamps
// arrive as nanoseconds since the start of the tape's epoch and are shown
// relative to the tape, because "at 00:14:02.317 into the session" is what an
// analyst can act on and a nanosecond count is not.

const NS_PER_MS = 1_000_000;
const NS_PER_SEC = 1_000_000_000;

export function formatPrice(minorUnits: number, decimals = 2): string {
  const negative = minorUnits < 0;
  const abs = Math.abs(minorUnits);
  const scale = 10 ** decimals;
  const whole = Math.floor(abs / scale);
  const frac = abs % scale;
  const text = `${whole}.${String(frac).padStart(decimals, "0")}`;
  return negative ? `-${text}` : text;
}

export function formatQty(qty: number): string {
  return qty.toLocaleString("en-US");
}

/** Wall-clock position within the tape, given the tape's first timestamp. */
export function formatTapeTime(ts: number, originTs: number): string {
  const offset = Math.max(0, ts - originTs);
  const totalMs = Math.floor(offset / NS_PER_MS);
  const ms = totalMs % 1000;
  const totalSec = Math.floor(totalMs / 1000);
  const s = totalSec % 60;
  const m = Math.floor(totalSec / 60) % 60;
  const h = Math.floor(totalSec / 3600);
  const pad = (n: number, w = 2) => String(n).padStart(w, "0");
  return `${pad(h)}:${pad(m)}:${pad(s)}.${pad(ms, 3)}`;
}

export function formatDurationNs(ns: number): string {
  if (ns < NS_PER_MS) return `${ns} ns`;
  if (ns < NS_PER_SEC) return `${(ns / NS_PER_MS).toFixed(0)} ms`;
  return `${(ns / NS_PER_SEC).toFixed(2)} s`;
}

export function formatScore(score: number): string {
  return score.toFixed(3);
}

export function detectorLabel(detector: string): string {
  return detector.replace(/_/g, " ");
}

export function evidenceLabel(key: string): string {
  return key.replace(/_/g, " ");
}

/**
 * Evidence values are integers whose units depend on the key. Rendering a
 * quantity and a millisecond count identically is how an analyst misreads a
 * 300 ms cancel as a 300 lot order.
 */
export function formatEvidence(key: string, value: number): string {
  if (key.endsWith("_ms")) return `${value.toLocaleString("en-US")} ms`;
  if (key.endsWith("_pct")) return `${value}%`;
  if (key.endsWith("_is_buy")) return value ? "buy" : "sell";
  if (key.startsWith("is_")) return value ? "yes" : "no";
  if (key.endsWith("_qty") || key.endsWith("_volume")) return value.toLocaleString("en-US");
  return value.toLocaleString("en-US");
}
