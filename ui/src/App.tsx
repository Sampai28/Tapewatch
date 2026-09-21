import { useCallback, useEffect, useMemo, useState } from "react";

import { AlertDetail } from "./components/AlertDetail";
import { AlertQueue } from "./components/AlertQueue";
import { BookReplay } from "./components/BookReplay";
import { CasePanel } from "./components/CasePanel";
import { Filters } from "./components/Filters";
import { SummaryBar } from "./components/SummaryBar";
import { api, ApiError } from "./lib/api";
import { applyQueue, emptyFilter, type QueueFilter } from "./lib/triage";
import type { AlertDetail as Alert, AlertRow, AlertStatus, CaseDetail, CaseRow, Summary } from "./types";

type Tab = "evidence" | "book" | "cases";

function message(e: unknown): string {
  return e instanceof ApiError ? e.message : String(e);
}

export default function App() {
  const [summary, setSummary] = useState<Summary | null>(null);
  const [alerts, setAlerts] = useState<AlertRow[]>([]);
  const [filter, setFilter] = useState<QueueFilter>(emptyFilter);
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [detail, setDetail] = useState<Alert | null>(null);
  const [checked, setChecked] = useState<Set<number>>(new Set());
  const [cases, setCases] = useState<CaseRow[]>([]);
  const [openCase, setOpenCase] = useState<CaseDetail | null>(null);
  const [tab, setTab] = useState<Tab>("evidence");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const reloadAlerts = useCallback(async () => {
    const r = await api.alerts({ limit: 1000, sort: "confidence" });
    setAlerts(r.alerts);
    return r.alerts;
  }, []);

  const reloadCases = useCallback(async () => {
    const r = await api.cases();
    setCases(r.cases);
  }, []);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const [s, list] = await Promise.all([api.summary(), api.alerts({ limit: 1000 })]);
        if (cancelled) return;
        setSummary(s);
        setAlerts(list.alerts);
        await reloadCases();
      } catch (e) {
        if (!cancelled) setError(message(e));
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [reloadCases]);

  const visible = useMemo(() => applyQueue(alerts, filter), [alerts, filter]);

  // The tape's own clock starts at whatever the first event's timestamp was.
  // Everything on screen is relative to it, so an analyst reads positions in
  // the session rather than nanosecond counts.
  const originTs = useMemo(
    () => (alerts.length === 0 ? 0 : Math.min(...alerts.map((a) => a.start_ts))),
    [alerts],
  );

  useEffect(() => {
    if (selectedId === null && visible.length > 0) {
      setSelectedId(visible[0]?.alert_id ?? null);
    }
  }, [visible, selectedId]);

  useEffect(() => {
    if (selectedId === null) {
      setDetail(null);
      return;
    }
    let cancelled = false;
    api
      .alert(selectedId)
      .then((d) => {
        if (!cancelled) setDetail(d);
      })
      .catch((e) => {
        if (!cancelled) setError(message(e));
      });
    return () => {
      cancelled = true;
    };
  }, [selectedId]);

  const toggleCheck = useCallback((id: number) => {
    setChecked((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }, []);

  const setAlertState = async (status: AlertStatus, disposition: string, note: string) => {
    if (selectedId === null) return;
    setBusy(true);
    setError(null);
    try {
      const updated = await api.setAlertState(selectedId, { status, disposition, note });
      setDetail(updated);
      setAlerts((prev) =>
        prev.map((a) =>
          a.alert_id === updated.alert_id
            ? { ...a, status: updated.status, disposition: updated.disposition }
            : a,
        ),
      );
      const s = await api.summary();
      setSummary(s);
    } catch (e) {
      setError(message(e));
    } finally {
      setBusy(false);
    }
  };

  const createCase = async (title: string, assignee: string) => {
    setBusy(true);
    setError(null);
    try {
      const created = await api.createCase({ title, assignee, alert_ids: [...checked] });
      setOpenCase(created);
      setChecked(new Set());
      await reloadCases();
      await reloadAlerts();
    } catch (e) {
      setError(message(e));
    } finally {
      setBusy(false);
    }
  };

  const attachToCase = async (caseId: number) => {
    setBusy(true);
    setError(null);
    try {
      const updated = await api.updateCase(caseId, { alert_ids: [...checked] });
      setOpenCase(updated);
      setChecked(new Set());
      await reloadCases();
      await reloadAlerts();
    } catch (e) {
      setError(message(e));
    } finally {
      setBusy(false);
    }
  };

  const setCaseStatus = async (caseId: number, status: string) => {
    setBusy(true);
    try {
      const updated = await api.updateCase(caseId, { status });
      setOpenCase(updated);
      await reloadCases();
    } catch (e) {
      setError(message(e));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="app">
      <header className="app-head">
        <h1>Tapewatch</h1>
        <p className="muted">market abuse surveillance &middot; analyst console</p>
        <SummaryBar summary={summary} />
      </header>

      {error ? (
        <p className="warn app-error" role="alert">
          {error}
        </p>
      ) : null}

      <main className="layout">
        <section className="pane pane-queue">
          <Filters
            filter={filter}
            total={alerts.length}
            shown={visible.length}
            onChange={setFilter}
          />
          <AlertQueue
            alerts={visible}
            selectedId={selectedId}
            originTs={originTs}
            checked={checked}
            onSelect={setSelectedId}
            onToggleCheck={toggleCheck}
          />
        </section>

        <section className="pane pane-detail">
          <nav className="tabs">
            {(["evidence", "book", "cases"] as Tab[]).map((t) => (
              <button
                key={t}
                type="button"
                className={tab === t ? "on" : ""}
                aria-pressed={tab === t}
                onClick={() => setTab(t)}
              >
                {t === "book" ? "book replay" : t}
              </button>
            ))}
          </nav>

          {tab === "cases" ? (
            <CasePanel
              cases={cases}
              open={openCase}
              selectedAlertIds={[...checked]}
              busy={busy}
              error={null}
              onCreate={createCase}
              onOpen={(id) => {
                api.case(id).then(setOpenCase).catch((e) => setError(message(e)));
              }}
              onAttach={attachToCase}
              onSetStatus={setCaseStatus}
            />
          ) : detail === null ? (
            <p className="empty">Select an alert.</p>
          ) : tab === "evidence" ? (
            <AlertDetail
              alert={detail}
              originTs={originTs}
              busy={busy}
              onSetState={setAlertState}
            />
          ) : (
            <BookReplay alert={detail} originTs={originTs} />
          )}
        </section>
      </main>
    </div>
  );
}
