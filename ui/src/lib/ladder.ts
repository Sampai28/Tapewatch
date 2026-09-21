// Turning replay frames into something drawable.
//
// The depth ladder has to keep a stable set of price rows across frames.
// Rebuilding the row set from each frame's own levels makes rows appear and
// disappear as liquidity moves, and the ladder jitters so badly that the one
// thing it exists to show -- a wall appearing and then vanishing -- becomes
// invisible. So the price range is computed once across the whole replay and
// every frame is projected onto it.

import type { Frame, Level } from "../types";

export interface LadderRow {
  price: number;
  bidQty: number;
  askQty: number;
  bidSubject: boolean;
  askSubject: boolean;
}

export interface LadderScale {
  prices: number[];
  maxQty: number;
}

/** Every price touched anywhere in the replay, descending, capped. */
export function ladderScale(frames: Frame[], maxRows = 24): LadderScale {
  const prices = new Set<number>();
  let maxQty = 1;
  for (const f of frames) {
    for (const l of [...f.bids, ...f.asks]) {
      prices.add(l.price);
      if (l.quantity > maxQty) maxQty = l.quantity;
    }
  }
  const sorted = [...prices].sort((a, b) => b - a);
  if (sorted.length <= maxRows) return { prices: sorted, maxQty };

  // Too many levels to show. Keep the ones around the middle of the range,
  // which is where the touch spends its time.
  const start = Math.max(0, Math.floor((sorted.length - maxRows) / 2));
  return { prices: sorted.slice(start, start + maxRows), maxQty };
}

export function ladderRows(frame: Frame | undefined, scale: LadderScale): LadderRow[] {
  const bids = new Map<number, Level>();
  const asks = new Map<number, Level>();
  if (frame) {
    for (const l of frame.bids) bids.set(l.price, l);
    for (const l of frame.asks) asks.set(l.price, l);
  }
  return scale.prices.map((price) => {
    const b = bids.get(price);
    const a = asks.get(price);
    return {
      price,
      bidQty: b?.quantity ?? 0,
      askQty: a?.quantity ?? 0,
      bidSubject: b?.subject ?? false,
      askSubject: a?.subject ?? false,
    };
  });
}

/** Index of the last frame at or before `ts`; 0 when the replay is empty. */
export function frameIndexAt(frames: Frame[], ts: number): number {
  if (frames.length === 0) return 0;
  let lo = 0;
  let hi = frames.length - 1;
  let best = 0;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const f = frames[mid];
    if (f === undefined) break;
    if (f.ts <= ts) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best;
}

export function describeFrame(frame: Frame): string {
  switch (frame.kind) {
    case "O":
      return `order ${frame.order_id}: ${frame.side === "B" ? "bid" : "offer"} ${frame.quantity}`;
    case "X":
      return `cancel ${frame.order_id}`;
    case "M":
      return `amend ${frame.order_id} to ${frame.quantity}`;
    case "T":
      return `trade ${frame.quantity} (${frame.side === "B" ? "buyer" : "seller"} aggressed)`;
    default:
      return "";
  }
}
