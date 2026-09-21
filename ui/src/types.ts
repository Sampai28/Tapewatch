// Wire types. These mirror what src/api/store.cpp emits; there is no codegen,
// so a change on one side without the other shows up as a test failure in
// src/lib/triage.test.ts rather than as undefined on screen.

export type Detector =
  | "spoofing"
  | "layering"
  | "wash_trading"
  | "momentum_ignition";

export type Outcome =
  | "true_positive"
  | "false_positive"
  | "misclassified"
  | "duplicate";

export type AlertStatus = "open" | "reviewing" | "closed";

export interface AlertRow {
  alert_id: number;
  detector: Detector;
  symbol: string;
  participant: number;
  counterparty: number | null;
  start_ts: number;
  end_ts: number;
  detect_ts: number;
  score: number;
  confidence: number;
  degraded: boolean;
  outcome: Outcome;
  role: string;
  above_operating_point: boolean;
  episode_id: number | null;
  status: AlertStatus;
  disposition: string;
  case_count: number;
  evidence: Record<string, number>;
}

export interface Episode {
  episode_id: number;
  kind: Detector;
  participant: number;
  start_ts: number;
  end_ts: number;
  intensity: number;
  detected: boolean;
  latency_ms: number;
  notes: Record<string, unknown>;
}

export interface AlertDetail extends AlertRow {
  penalty: number;
  note: string;
  signals: Record<string, number>;
  weights: Record<string, number>;
  order_ids: number[];
  episode: Episode | null;
  cases: { case_id: number; title: string; status: string }[];
}

export interface Summary {
  meta: Record<string, string>;
  counts: {
    alerts: number;
    alerts_above_operating_point: number;
    episodes: number;
    episodes_detected: number;
    open: number;
    cases: number;
  };
  by_detector: {
    detector: Detector;
    alerts: number;
    true_positives: number;
    false_positives: number;
    above_operating_point: number;
  }[];
}

export interface Level {
  price: number;
  quantity: number;
  orders: number;
  subject: boolean;
  subject_qty: number;
}

export interface Frame {
  ts: number;
  seq: number;
  kind: "O" | "X" | "M" | "T";
  participant: number;
  order_id: number;
  side: "B" | "S";
  price: number;
  quantity: number;
  maker_participant?: number;
  maker_order_id?: number;
  best_bid: number | null;
  best_ask: number | null;
  highlighted: boolean;
  bids: Level[];
  asks: Level[];
}

export interface ReplayResponse {
  symbol: string;
  from_ts: number;
  to_ts: number;
  events_in_window: number;
  frame_stride: number;
  frames: Frame[];
}

export interface CaseRow {
  case_id: number;
  title: string;
  status: string;
  assignee: string;
  summary: string;
  created_ts: number;
  updated_ts: number;
  alert_count: number;
}

export interface CaseDetail extends Omit<CaseRow, "alert_count"> {
  alerts: {
    alert_id: number;
    detector: Detector;
    participant: number;
    score: number;
    confidence: number;
    outcome: Outcome;
    detect_ts: number;
  }[];
}

export interface Problem {
  title: string;
  status: number;
  detail: string;
}
