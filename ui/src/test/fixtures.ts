import type { AlertDetail, AlertRow, Frame } from "../types";

const SEC = 1_000_000_000;

export function alertRow(over: Partial<AlertRow> = {}): AlertRow {
  return {
    alert_id: 1,
    detector: "spoofing",
    symbol: "TWX",
    participant: 1031,
    counterparty: null,
    start_ts: 10 * SEC,
    end_ts: 12 * SEC,
    detect_ts: 13 * SEC,
    score: 0.7,
    confidence: 0.7,
    degraded: false,
    outcome: "true_positive",
    role: "spoofer",
    above_operating_point: true,
    episode_id: 4,
    status: "open",
    disposition: "",
    case_count: 0,
    evidence: { cancelled_qty: 240, lifetime_ms: 420, opposite_qty: 60 },
    ...over,
  };
}

export function alertDetail(over: Partial<AlertDetail> = {}): AlertDetail {
  return {
    ...alertRow(),
    penalty: 0,
    note: "",
    signals: { size: 0.9, life: 0.8, unfilled: 1, opposite: 0.4, press: 1 },
    weights: { size: 0.28, life: 0.18, unfilled: 0.12, opposite: 0.3, press: 0.12 },
    order_ids: [5001],
    episode: {
      episode_id: 4,
      kind: "spoofing",
      participant: 1031,
      start_ts: 10 * SEC,
      end_ts: 12 * SEC,
      intensity: 0.82,
      detected: true,
      latency_ms: 3000,
      notes: {},
    },
    cases: [],
    ...over,
  };
}

export function frame(over: Partial<Frame> = {}): Frame {
  return {
    ts: 10 * SEC,
    seq: 1,
    kind: "O",
    participant: 1031,
    order_id: 5001,
    side: "B",
    price: 10000,
    quantity: 240,
    best_bid: 10000,
    best_ask: 10004,
    highlighted: false,
    bids: [{ price: 10000, quantity: 240, orders: 1, subject: true, subject_qty: 240 }],
    asks: [{ price: 10004, quantity: 30, orders: 1, subject: false, subject_qty: 0 }],
    ...over,
  };
}
