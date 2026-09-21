import { detectorLabel } from "../lib/format";
import { ALL_DETECTORS, type QueueFilter, type SortKey } from "../lib/triage";
import type { Detector } from "../types";

interface Props {
  filter: QueueFilter;
  total: number;
  shown: number;
  onChange: (next: QueueFilter) => void;
}

export function Filters({ filter, total, shown, onChange }: Props) {
  const toggleDetector = (d: Detector) => {
    const next = new Set(filter.detectors);
    if (next.has(d)) next.delete(d);
    else next.add(d);
    onChange({ ...filter, detectors: next });
  };

  return (
    <div className="filters">
      <div className="filter-row">
        <input
          type="search"
          placeholder="participant, detector, alert id"
          aria-label="search alerts"
          value={filter.search}
          onChange={(e) => onChange({ ...filter, search: e.target.value })}
        />
        <select
          aria-label="sort order"
          value={filter.sort}
          onChange={(e) => onChange({ ...filter, sort: e.target.value as SortKey })}
        >
          <option value="confidence">confidence</option>
          <option value="score">raw score</option>
          <option value="recent">newest first</option>
          <option value="oldest">oldest first</option>
        </select>
      </div>

      <div className="filter-row chips">
        {ALL_DETECTORS.map((d) => (
          <button
            key={d}
            type="button"
            className={`chip ${filter.detectors.has(d) ? "on" : ""}`}
            aria-pressed={filter.detectors.has(d)}
            onClick={() => toggleDetector(d)}
          >
            {detectorLabel(d)}
          </button>
        ))}
      </div>

      <div className="filter-row chips">
        {(["all", "open", "reviewing", "closed"] as const).map((s) => (
          <button
            key={s}
            type="button"
            className={`chip ${filter.status === s ? "on" : ""}`}
            aria-pressed={filter.status === s}
            onClick={() => onChange({ ...filter, status: s })}
          >
            {s}
          </button>
        ))}
      </div>

      <div className="filter-row">
        <label>
          <input
            type="checkbox"
            checked={filter.onlyTuned}
            onChange={(e) => onChange({ ...filter, onlyTuned: e.target.checked })}
          />
          only above the operating point
        </label>
        <label>
          <input
            type="checkbox"
            checked={filter.hideDegraded}
            onChange={(e) => onChange({ ...filter, hideDegraded: e.target.checked })}
          />
          hide degraded-feed alerts
        </label>
      </div>

      <p className="filter-count">
        {shown} of {total} alerts
      </p>
    </div>
  );
}
