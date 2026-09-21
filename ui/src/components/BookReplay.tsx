import { useEffect, useMemo, useRef, useState } from "react";

import { api, ApiError } from "../lib/api";
import { formatPrice, formatQty, formatTapeTime } from "../lib/format";
import { describeFrame, frameIndexAt, ladderRows, ladderScale } from "../lib/ladder";
import type { AlertDetail, ReplayResponse } from "../types";

interface Props {
  alert: AlertDetail;
  originTs: number;
}

const PAD_NS = 2_000_000_000; // two seconds either side of the alert window

export function BookReplay({ alert, originTs }: Props) {
  const [data, setData] = useState<ReplayResponse | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [index, setIndex] = useState(0);
  const [playing, setPlaying] = useState(false);
  const timer = useRef<number | null>(null);

  useEffect(() => {
    let cancelled = false;
    setData(null);
    setError(null);
    setIndex(0);
    setPlaying(false);
    api
      .replay({
        symbol: alert.symbol,
        from_ts: alert.start_ts - PAD_NS,
        to_ts: alert.end_ts + PAD_NS,
        depth: 12,
        max_frames: 300,
        order_ids: alert.order_ids,
      })
      .then((r) => {
        if (cancelled) return;
        setData(r);
        // Open on the first frame that touches one of the alert's own orders,
        // not on the start of the padding. That is the moment the analyst is
        // here to look at.
        const first = r.frames.findIndex((f) => f.highlighted);
        setIndex(first >= 0 ? first : frameIndexAt(r.frames, alert.start_ts));
      })
      .catch((e: unknown) => {
        if (!cancelled) {
          setError(e instanceof ApiError ? e.message : String(e));
        }
      });
    return () => {
      cancelled = true;
    };
  }, [alert.alert_id, alert.symbol, alert.start_ts, alert.end_ts, alert.order_ids]);

  useEffect(() => {
    if (!playing || !data) return;
    timer.current = window.setInterval(() => {
      setIndex((i) => {
        if (i + 1 >= data.frames.length) {
          setPlaying(false);
          return i;
        }
        return i + 1;
      });
    }, 90);
    return () => {
      if (timer.current !== null) window.clearInterval(timer.current);
    };
  }, [playing, data]);

  const scale = useMemo(() => (data ? ladderScale(data.frames) : { prices: [], maxQty: 1 }), [data]);
  const frame = data?.frames[index];
  const rows = useMemo(() => ladderRows(frame, scale), [frame, scale]);

  if (error) {
    return (
      <div className="replay">
        <p className="warn">Book replay unavailable: {error}</p>
      </div>
    );
  }
  if (!data) return <p className="muted">Reconstructing the book&hellip;</p>;
  if (data.frames.length === 0) {
    return <p className="muted">No events on this symbol inside the alert window.</p>;
  }

  return (
    <div className="replay">
      <div className="replay-controls">
        <button type="button" onClick={() => setPlaying((p) => !p)}>
          {playing ? "Pause" : "Play"}
        </button>
        <button type="button" onClick={() => setIndex((i) => Math.max(0, i - 1))}>
          Back
        </button>
        <button
          type="button"
          onClick={() => setIndex((i) => Math.min(data.frames.length - 1, i + 1))}
        >
          Forward
        </button>
        <input
          type="range"
          min={0}
          max={data.frames.length - 1}
          value={index}
          aria-label="replay position"
          onChange={(e) => {
            setPlaying(false);
            setIndex(Number(e.target.value));
          }}
        />
        <span className="muted">
          {index + 1} / {data.frames.length}
          {data.frame_stride > 1 ? ` (every ${data.frame_stride} events)` : ""}
        </span>
      </div>

      {frame ? (
        <p className="replay-caption">
          <strong>{formatTapeTime(frame.ts, originTs)}</strong> &middot; participant{" "}
          {frame.participant} &middot; {describeFrame(frame)}
          {frame.highlighted ? " — this alert's order" : ""}
        </p>
      ) : null}

      <table className="ladder">
        <thead>
          <tr>
            <th>bid</th>
            <th>price</th>
            <th>offer</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => {
            const isTouchBid = frame?.best_bid === r.price;
            const isTouchAsk = frame?.best_ask === r.price;
            return (
              <tr key={r.price} className={isTouchBid || isTouchAsk ? "touch" : ""}>
                <td className="bid">
                  {r.bidQty > 0 ? (
                    <span className={`depth ${r.bidSubject ? "subject" : ""}`}>
                      <span
                        className="depth-bar"
                        style={{ width: `${(r.bidQty / scale.maxQty) * 100}%` }}
                      />
                      <span className="depth-qty">{formatQty(r.bidQty)}</span>
                    </span>
                  ) : null}
                </td>
                <td className="price">{formatPrice(r.price)}</td>
                <td className="ask">
                  {r.askQty > 0 ? (
                    <span className={`depth ${r.askSubject ? "subject" : ""}`}>
                      <span
                        className="depth-bar"
                        style={{ width: `${(r.askQty / scale.maxQty) * 100}%` }}
                      />
                      <span className="depth-qty">{formatQty(r.askQty)}</span>
                    </span>
                  ) : null}
                </td>
              </tr>
            );
          })}
        </tbody>
      </table>
      <p className="muted">
        Highlighted depth is quantity belonging to this alert&rsquo;s own orders. The window
        covers {data.events_in_window.toLocaleString("en-US")} events on {data.symbol}.
      </p>
    </div>
  );
}
