import type { Summary } from "../types";

interface Props {
  summary: Summary | null;
}

function stat(label: string, value: string, note?: string) {
  return (
    <div className="stat" key={label}>
      <div className="stat-k">{label}</div>
      <div className="stat-v">{value}</div>
      {note ? <div className="stat-n">{note}</div> : null}
    </div>
  );
}

export function SummaryBar({ summary }: Props) {
  if (!summary) {
    return (
      <div className="summary">
        <div className="stat">
          <div className="stat-k">loading</div>
          <div className="stat-v">&hellip;</div>
        </div>
      </div>
    );
  }
  const m = summary.meta;
  const c = summary.counts;
  const num = (key: string, digits = 3) => {
    const v = Number(m[key]);
    return Number.isFinite(v) ? v.toFixed(digits) : "--";
  };
  return (
    <div className="summary">
      {stat("precision", num("precision"), "at the operating point")}
      {stat("recall", num("recall"), `${c.episodes_detected}/${c.episodes} episodes`)}
      {stat("F1", num("f1"))}
      {stat("alerts", `${c.alerts_above_operating_point}`, `${c.alerts} before thresholding`)}
      {stat("MM false positives", num("mm_fp_per_mm_hour", 2), "per market maker per hour")}
      {stat("open", `${c.open}`, `${c.cases} cases`)}
    </div>
  );
}
