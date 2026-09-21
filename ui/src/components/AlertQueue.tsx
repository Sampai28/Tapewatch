import { memo } from "react";

import { detectorLabel, formatScore, formatTapeTime } from "../lib/format";
import type { AlertRow } from "../types";

interface Props {
  alerts: AlertRow[];
  selectedId: number | null;
  originTs: number;
  checked: Set<number>;
  onSelect: (id: number) => void;
  onToggleCheck: (id: number) => void;
}

interface RowProps {
  alert: AlertRow;
  selected: boolean;
  checked: boolean;
  originTs: number;
  onSelect: (id: number) => void;
  onToggleCheck: (id: number) => void;
}

// An explicit comparator, because the queue re-renders on every selection
// change and the rows are otherwise identical. Without it, selecting one
// alert re-renders all several hundred.
const Row = memo(
  function Row({ alert, selected, checked, originTs, onSelect, onToggleCheck }: RowProps) {
    const cls = [
      "queue-row",
      selected ? "is-selected" : "",
      alert.status === "closed" ? "is-closed" : "",
      alert.degraded ? "is-degraded" : "",
    ]
      .filter(Boolean)
      .join(" ");
    return (
      <div
        className={cls}
        role="option"
        aria-selected={selected}
        tabIndex={0}
        onClick={() => onSelect(alert.alert_id)}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            onSelect(alert.alert_id);
          }
        }}
      >
        <input
          type="checkbox"
          checked={checked}
          aria-label={`select alert ${alert.alert_id}`}
          onClick={(e) => e.stopPropagation()}
          onChange={() => onToggleCheck(alert.alert_id)}
        />
        <span className={`pill pill-${alert.detector}`}>{detectorLabel(alert.detector)}</span>
        <span className="queue-participant">#{alert.participant}</span>
        <span className="queue-time">{formatTapeTime(alert.detect_ts, originTs)}</span>
        <span className="queue-score" title={`raw score ${formatScore(alert.score)}`}>
          {formatScore(alert.confidence)}
        </span>
        {alert.degraded ? <span className="flag" title="raised over a damaged feed">feed</span> : null}
        {alert.case_count > 0 ? <span className="flag">case</span> : null}
        {alert.status !== "open" ? <span className="flag">{alert.status}</span> : null}
      </div>
    );
  },
  (a, b) =>
    a.alert.alert_id === b.alert.alert_id &&
    a.alert.status === b.alert.status &&
    a.alert.case_count === b.alert.case_count &&
    a.selected === b.selected &&
    a.checked === b.checked &&
    a.originTs === b.originTs,
);

export function AlertQueue({
  alerts,
  selectedId,
  originTs,
  checked,
  onSelect,
  onToggleCheck,
}: Props) {
  if (alerts.length === 0) {
    return <p className="empty">No alerts match these filters.</p>;
  }
  return (
    <div className="queue" role="listbox" aria-label="alert queue">
      {alerts.map((a) => (
        <Row
          key={a.alert_id}
          alert={a}
          selected={a.alert_id === selectedId}
          checked={checked.has(a.alert_id)}
          originTs={originTs}
          onSelect={onSelect}
          onToggleCheck={onToggleCheck}
        />
      ))}
    </div>
  );
}
