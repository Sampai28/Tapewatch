import { useState } from "react";

import { detectorLabel, formatScore } from "../lib/format";
import type { CaseDetail, CaseRow } from "../types";

interface Props {
  cases: CaseRow[];
  open: CaseDetail | null;
  selectedAlertIds: number[];
  busy: boolean;
  error: string | null;
  onCreate: (title: string, assignee: string) => void;
  onOpen: (caseId: number) => void;
  onAttach: (caseId: number) => void;
  onSetStatus: (caseId: number, status: string) => void;
}

export function CasePanel({
  cases,
  open,
  selectedAlertIds,
  busy,
  error,
  onCreate,
  onOpen,
  onAttach,
  onSetStatus,
}: Props) {
  const [title, setTitle] = useState("");
  const [assignee, setAssignee] = useState("");

  return (
    <div className="cases">
      <h3>Cases</h3>
      <p className="muted">
        Several alerts on one participant over a few minutes are one investigation. Tick
        alerts in the queue and group them here; the alert count is what a surveillance
        system is judged on, the case count is what the desk actually works.
      </p>

      {error ? <p className="warn">{error}</p> : null}

      <div className="case-create">
        <input
          type="text"
          placeholder="case title"
          aria-label="case title"
          value={title}
          onChange={(e) => setTitle(e.target.value)}
        />
        <input
          type="text"
          placeholder="assignee"
          aria-label="assignee"
          value={assignee}
          onChange={(e) => setAssignee(e.target.value)}
        />
        <button
          type="button"
          disabled={busy || title.trim() === "" || selectedAlertIds.length === 0}
          onClick={() => {
            onCreate(title.trim(), assignee.trim());
            setTitle("");
          }}
        >
          Create from {selectedAlertIds.length} selected
        </button>
      </div>

      {cases.length === 0 ? (
        <p className="empty">No cases yet.</p>
      ) : (
        <table className="case-table">
          <thead>
            <tr>
              <th>case</th>
              <th>title</th>
              <th>status</th>
              <th>alerts</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {cases.map((c) => (
              <tr key={c.case_id} className={open?.case_id === c.case_id ? "is-selected" : ""}>
                <td>{c.case_id}</td>
                <td>
                  <button type="button" className="linky" onClick={() => onOpen(c.case_id)}>
                    {c.title}
                  </button>
                  {c.assignee ? <span className="muted"> &middot; {c.assignee}</span> : null}
                </td>
                <td>{c.status}</td>
                <td className="num">{c.alert_count}</td>
                <td>
                  <button
                    type="button"
                    disabled={busy || selectedAlertIds.length === 0}
                    onClick={() => onAttach(c.case_id)}
                  >
                    add {selectedAlertIds.length}
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {open ? (
        <div className="case-detail">
          <h4>
            Case {open.case_id}: {open.title}
          </h4>
          <div className="triage-actions">
            {["open", "escalated", "closed"].map((s) => (
              <button
                key={s}
                type="button"
                disabled={busy || open.status === s}
                onClick={() => onSetStatus(open.case_id, s)}
              >
                {s}
              </button>
            ))}
            <span className="muted">current: {open.status}</span>
          </div>
          <table className="case-table">
            <thead>
              <tr>
                <th>alert</th>
                <th>detector</th>
                <th>participant</th>
                <th>confidence</th>
              </tr>
            </thead>
            <tbody>
              {open.alerts.map((a) => (
                <tr key={a.alert_id}>
                  <td>{a.alert_id}</td>
                  <td>{detectorLabel(a.detector)}</td>
                  <td className="num">{a.participant}</td>
                  <td className="num">{formatScore(a.confidence)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : null}
    </div>
  );
}
