import { useState } from "react";

import {
  detectorLabel,
  evidenceLabel,
  formatDurationNs,
  formatEvidence,
  formatScore,
  formatTapeTime,
} from "../lib/format";
import { outcomeLabel } from "../lib/triage";
import type { AlertDetail as Alert, AlertStatus } from "../types";

interface Props {
  alert: Alert;
  originTs: number;
  busy: boolean;
  onSetState: (status: AlertStatus, disposition: string, note: string) => void;
}

/**
 * Signal bars. The point is that an analyst can see in one glance *which*
 * component carried the score -- a spoofing alert scoring 0.7 entirely on
 * size is a different thing from one scoring 0.7 on the opposite-side trade,
 * and only the second is worth opening.
 */
function SignalBars({ signals, weights }: { signals: Record<string, number>; weights: Record<string, number> }) {
  const names = Object.keys(signals).sort(
    (a, b) => (signals[b] ?? 0) * (weights[b] ?? 0) - (signals[a] ?? 0) * (weights[a] ?? 0),
  );
  return (
    <table className="signals">
      <thead>
        <tr>
          <th>signal</th>
          <th>value</th>
          <th>weight</th>
          <th>contribution</th>
        </tr>
      </thead>
      <tbody>
        {names.map((name) => {
          const value = signals[name] ?? 0;
          const weight = weights[name] ?? 0;
          return (
            <tr key={name}>
              <td>{evidenceLabel(name)}</td>
              <td className="num">{value.toFixed(3)}</td>
              <td className="num">{weight.toFixed(2)}</td>
              <td className="bar-cell">
                <span className="bar" style={{ width: `${Math.round(value * weight * 200)}px` }} />
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

export function AlertDetail({ alert, originTs, busy, onSetState }: Props) {
  const [disposition, setDisposition] = useState(alert.disposition);
  const [note, setNote] = useState(alert.note);

  const act = (status: AlertStatus) => onSetState(status, disposition, note);

  return (
    <div className="detail">
      <header className="detail-head">
        <div>
          <span className={`pill pill-${alert.detector}`}>{detectorLabel(alert.detector)}</span>
          <h2>
            Alert {alert.alert_id} &middot; participant {alert.participant}
            {alert.counterparty !== null && alert.counterparty !== alert.participant
              ? ` and ${alert.counterparty}`
              : ""}
          </h2>
          <p className="muted">
            {alert.symbol} &middot; {formatTapeTime(alert.start_ts, originTs)} to{" "}
            {formatTapeTime(alert.end_ts, originTs)} &middot; lasted{" "}
            {formatDurationNs(alert.end_ts - alert.start_ts)} &middot; raised at{" "}
            {formatTapeTime(alert.detect_ts, originTs)}
          </p>
        </div>
        <div className="detail-score">
          <div className="big">{formatScore(alert.confidence)}</div>
          <div className="muted">
            confidence
            {alert.degraded ? ` (score ${formatScore(alert.score)}, cut for feed damage)` : ""}
          </div>
          {alert.penalty > 0 ? (
            <div className="muted">penalty applied &minus;{alert.penalty.toFixed(3)}</div>
          ) : null}
        </div>
      </header>

      {alert.degraded ? (
        <p className="warn">
          The feed was damaged near this window &mdash; a sequence gap, a duplicate or a
          timestamp conflict. The book reconstruction may be incomplete and the confidence has
          been reduced accordingly.
        </p>
      ) : null}

      <h3>Why it fired</h3>
      <SignalBars signals={alert.signals} weights={alert.weights} />

      <h3>Evidence</h3>
      <table className="evidence">
        <tbody>
          {Object.entries(alert.evidence).map(([k, v]) => (
            <tr key={k}>
              <td>{evidenceLabel(k)}</td>
              <td className="num">{formatEvidence(k, v)}</td>
            </tr>
          ))}
        </tbody>
      </table>
      {alert.order_ids.length > 0 ? (
        <p className="muted">
          Orders: {alert.order_ids.slice(0, 12).join(", ")}
          {alert.order_ids.length > 12 ? ` and ${alert.order_ids.length - 12} more` : ""}
        </p>
      ) : null}

      <h3>Ground truth</h3>
      <p className="muted lab">
        Only a laboratory has this. It is shown so the queue can be judged &mdash; a ranking
        is only as good as what sits at the top of it &mdash; and it is not available to a
        real surveillance desk at triage time.
      </p>
      {alert.episode ? (
        <table className="evidence">
          <tbody>
            <tr>
              <td>verdict</td>
              <td className="num">{outcomeLabel(alert.outcome)}</td>
            </tr>
            <tr>
              <td>injected episode</td>
              <td className="num">
                {alert.episode.episode_id} ({detectorLabel(alert.episode.kind)})
              </td>
            </tr>
            <tr>
              <td>intensity</td>
              <td className="num">{alert.episode.intensity.toFixed(2)}</td>
            </tr>
            <tr>
              <td>detection latency</td>
              <td className="num">{alert.episode.latency_ms.toLocaleString("en-US")} ms</td>
            </tr>
            <tr>
              <td>participant role</td>
              <td className="num">{alert.role}</td>
            </tr>
          </tbody>
        </table>
      ) : (
        <table className="evidence">
          <tbody>
            <tr>
              <td>verdict</td>
              <td className="num">{outcomeLabel(alert.outcome)}</td>
            </tr>
            <tr>
              <td>participant role</td>
              <td className="num">{alert.role}</td>
            </tr>
          </tbody>
        </table>
      )}

      <h3>Triage</h3>
      <div className="triage">
        <input
          type="text"
          placeholder="disposition, e.g. market making, escalate, insufficient evidence"
          aria-label="disposition"
          value={disposition}
          onChange={(e) => setDisposition(e.target.value)}
        />
        <textarea
          placeholder="notes"
          aria-label="notes"
          rows={3}
          value={note}
          onChange={(e) => setNote(e.target.value)}
        />
        <div className="triage-actions">
          <button type="button" disabled={busy} onClick={() => act("reviewing")}>
            Mark reviewing
          </button>
          <button type="button" disabled={busy} onClick={() => act("closed")}>
            Close
          </button>
          <button type="button" disabled={busy} onClick={() => act("open")}>
            Reopen
          </button>
          <span className="muted">current: {alert.status}</span>
        </div>
      </div>
    </div>
  );
}
