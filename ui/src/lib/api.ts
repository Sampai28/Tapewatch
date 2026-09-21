// The one place that talks to the server.
//
// Every failure comes back as an Error carrying the RFC 7807 title and
// detail, so components have exactly one error shape to render. A back end
// that sometimes returns a string and sometimes an object is how a front end
// ends up with three error paths and two of them untested.

import type {
  AlertDetail,
  AlertRow,
  AlertStatus,
  CaseDetail,
  CaseRow,
  Problem,
  ReplayResponse,
  Summary,
} from "../types";

export class ApiError extends Error {
  readonly status: number;
  constructor(status: number, message: string) {
    super(message);
    this.name = "ApiError";
    this.status = status;
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    res = await fetch(path, {
      ...init,
      headers: { "Content-Type": "application/json", ...(init?.headers ?? {}) },
    });
  } catch (cause) {
    throw new ApiError(0, `cannot reach the API (${String(cause)})`);
  }
  const text = await res.text();
  if (!res.ok) {
    let message = `${res.status} ${res.statusText}`;
    try {
      const problem = JSON.parse(text) as Problem;
      if (problem.title) message = problem.detail ? `${problem.title}: ${problem.detail}` : problem.title;
    } catch {
      if (text) message = text.slice(0, 200);
    }
    throw new ApiError(res.status, message);
  }
  return (text ? JSON.parse(text) : {}) as T;
}

function query(params: Record<string, string | number | boolean | null | undefined>): string {
  const q = new URLSearchParams();
  for (const [k, v] of Object.entries(params)) {
    if (v === null || v === undefined || v === "" || v === false) continue;
    q.set(k, v === true ? "1" : String(v));
  }
  const s = q.toString();
  return s ? `?${s}` : "";
}

export const api = {
  summary: () => request<Summary>("/api/summary"),

  alerts: (opts: {
    detector?: string;
    symbol?: string;
    status?: string;
    participant?: number | null;
    tuned?: boolean;
    limit?: number;
    offset?: number;
    sort?: string;
  } = {}) => request<{ alerts: AlertRow[] }>(`/api/alerts${query(opts)}`),

  alert: (id: number) => request<AlertDetail>(`/api/alerts/${id}`),

  setAlertState: (id: number, body: { status: AlertStatus; disposition?: string; note?: string }) =>
    request<AlertDetail>(`/api/alerts/${id}/state`, {
      method: "POST",
      body: JSON.stringify({ disposition: "", note: "", ...body }),
    }),

  cases: () => request<{ cases: CaseRow[] }>("/api/cases"),

  case: (id: number) => request<CaseDetail>(`/api/cases/${id}`),

  createCase: (body: { title: string; assignee?: string; summary?: string; alert_ids: number[] }) =>
    request<CaseDetail>("/api/cases", { method: "POST", body: JSON.stringify(body) }),

  updateCase: (
    id: number,
    body: { status?: string; assignee?: string; summary?: string; alert_ids?: number[] },
  ) => request<CaseDetail>(`/api/cases/${id}`, { method: "POST", body: JSON.stringify(body) }),

  replay: (opts: {
    symbol: string;
    from_ts: number;
    to_ts: number;
    depth?: number;
    max_frames?: number;
    order_ids?: number[];
  }) =>
    request<ReplayResponse>(
      `/api/replay${query({
        symbol: opts.symbol,
        from_ts: opts.from_ts,
        to_ts: opts.to_ts,
        depth: opts.depth,
        max_frames: opts.max_frames,
        order_ids: opts.order_ids?.join(","),
      })}`,
    ),
};
