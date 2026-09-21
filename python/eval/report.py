"""Render eval.json as one self-contained HTML page.

    python -m eval.report --run results/small --out reports/small.html

No template engine and no chart library: the whole page is string formatting
and inline SVG, which keeps the Python side dependency-free and makes the
output a single file that survives being emailed.

Every number on the page comes from eval.json, which comes from a run. There
are no placeholders and nothing is filled in by hand -- if a figure is
missing from the page it is because the run did not produce it.
"""

from __future__ import annotations

import argparse
import html
import json
import os
from typing import Any, Iterable, Sequence

from tapewatch import svg

CSS = """
:root { --ink:#22201d; --muted:#6b6862; --rule:#ddd9d1; --bg:#faf9f6;
        --good:#2f6f4f; --bad:#a3382c; }
* { box-sizing: border-box; }
body { margin:0; padding:2.2rem 1.4rem 4rem; background:var(--bg); color:var(--ink);
       font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,sans-serif; }
.wrap { max-width: 1080px; margin: 0 auto; }
h1 { font-size:1.7rem; margin:0 0 .2rem; letter-spacing:-.01em; }
h2 { font-size:1.12rem; margin:2.4rem 0 .6rem; padding-bottom:.3rem;
     border-bottom:1px solid var(--rule); }
h3 { font-size:.95rem; margin:1.4rem 0 .4rem; color:var(--muted);
     text-transform:uppercase; letter-spacing:.06em; }
p { margin:.5rem 0; max-width:74ch; }
.sub { color:var(--muted); margin:0 0 1.4rem; font-size:.9rem; }
table { border-collapse:collapse; width:100%; margin:.6rem 0 1rem; font-size:.88rem; }
th,td { text-align:right; padding:.36rem .55rem; border-bottom:1px solid var(--rule); }
th:first-child, td:first-child { text-align:left; }
thead th { font-weight:600; color:var(--muted); font-size:.78rem;
           text-transform:uppercase; letter-spacing:.05em; }
tbody tr:hover { background:#f2f0ea; }
.cards { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:.7rem;
         margin:1rem 0 1.6rem; }
.card { background:#fff; border:1px solid var(--rule); border-radius:5px; padding:.7rem .85rem; }
.card .k { font-size:.72rem; text-transform:uppercase; letter-spacing:.06em;
           color:var(--muted); }
.card .v { font-size:1.45rem; font-variant-numeric:tabular-nums; margin-top:.15rem; }
.card .n { font-size:.76rem; color:var(--muted); }
.charts { display:grid; grid-template-columns:repeat(auto-fit,minmax(340px,1fr)); gap:1rem; }
.chart { background:#fff; border:1px solid var(--rule); border-radius:5px; padding:.6rem; }
.chart h4 { margin:.1rem .3rem .4rem; font-size:.85rem; font-weight:600; }
.note { background:#fff; border-left:3px solid var(--muted); padding:.6rem .9rem;
        margin:.9rem 0; font-size:.88rem; }
.pos { color:var(--good); } .neg { color:var(--bad); }
code { font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,monospace; background:#f0eee8;
       padding:.05rem .3rem; border-radius:3px; }
footer { margin-top:3rem; color:var(--muted); font-size:.8rem; }
"""


def esc(v: Any) -> str:
    return html.escape(str(v))


def table(headers: Sequence[str], rows: Iterable[Sequence[Any]]) -> str:
    head = "".join(f"<th>{esc(h)}</th>" for h in headers)
    body = "".join(
        "<tr>" + "".join(f"<td>{cell if isinstance(cell, str) and cell.startswith('<') else esc(cell)}</td>"
                         for cell in row) + "</tr>"
        for row in rows
    )
    return f"<table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>"


def card(key: str, value: Any, note: str = "") -> str:
    return (f'<div class="card"><div class="k">{esc(key)}</div>'
            f'<div class="v">{esc(value)}</div>'
            f'<div class="n">{esc(note)}</div></div>')


def pct(x: float) -> str:
    return f"{x * 100:.1f}%"


def delta(x: float) -> str:
    cls = "pos" if x > 0 else ("neg" if x < 0 else "")
    sign = "+" if x > 0 else ""
    return f'<span class="{cls}">{sign}{x:+.3f}</span>'.replace("++", "+")


def render(ev: dict[str, Any]) -> str:
    tuned = ev["tuned"]
    raw = ev["raw"]
    o = tuned["overall"]
    mm = tuned["market_maker"]
    sweep = ev["sweep"]
    detectors = sorted(tuned["detectors"])

    parts: list[str] = []
    parts.append("<!doctype html><meta charset='utf-8'>")
    parts.append("<meta name='viewport' content='width=device-width,initial-scale=1'>")
    parts.append("<title>Tapewatch surveillance report</title>")
    parts.append(f"<style>{CSS}</style><div class='wrap'>")

    parts.append("<h1>Tapewatch surveillance report</h1>")
    parts.append(
        f"<p class='sub'>{esc(ev['run_dir'])} &middot; "
        f"{ev['tape_seconds'] / 60:.1f} minutes of tape &middot; "
        f"{ev['engine']['events']:,} events &middot; "
        f"{ev['counts']['episodes']} injected episodes</p>"
    )

    # ---- headline ------------------------------------------------------
    parts.append("<div class='cards'>")
    parts.append(card("precision", f"{o['precision']:.3f}", "at chosen operating points"))
    parts.append(card("recall", f"{o['recall']:.3f}", f"{o['true_positives']}/{o['episodes']}"))
    parts.append(card("F1", f"{o['f1']:.3f}", ""))
    parts.append(card("latency p50", f"{o['latency_ms']['p50']:.0f} ms",
                      f"p90 {o['latency_ms']['p90']:.0f} ms"))
    parts.append(card("alerts / hour", f"{o['alerts_per_hour']:.0f}",
                      f"{tuned['workload']['analyst_hours_per_market_hour']:.2f} analyst-hours "
                      f"per market hour"))
    parts.append(card("MM false positives", f"{mm['per_mm_per_hour']:.2f}",
                      f"per market maker per hour ({mm['false_positives']} total)"))
    parts.append("</div>")

    # ---- market makers first, because it is the point ------------------
    parts.append("<h2>Market makers</h2>")
    parts.append(
        "<p>Legitimate market makers are the hardest false positive in market abuse "
        "surveillance and the reason surveillance systems get switched off. They cancel "
        "constantly, quote both sides, hold an enormous order-to-trade ratio, and -- when "
        "inventory runs away from them -- pull the quotes on one side and cross the spread "
        "to flatten, which is the textbook shape of spoofing performed by somebody doing "
        "nothing wrong. The engine is never told who they are: the roster lives in the "
        "generator's ground truth and is read only here.</p>"
    )
    mm_rows = []
    for name in detectors:
        d = tuned["detectors"][name]
        r = raw["detectors"][name]
        mm_rows.append([
            name, r["false_positives"], r["mm_false_positives"],
            d["false_positives"], d["mm_false_positives"],
            pct(d["mm_false_positives"] / d["false_positives"]) if d["false_positives"] else "--",
        ])
    parts.append(table(
        ["detector", "FP untuned", "of which MM", "FP tuned", "of which MM", "MM share"],
        mm_rows,
    ))
    parts.append(
        f"<div class='note'>Across all detectors, market makers account for "
        f"<strong>{mm['false_positives']}</strong> of "
        f"{o['false_positives']} false positives "
        f"({pct(mm['share_of_false_positives'])}), which is "
        f"<strong>{mm['per_mm_per_hour']:.2f}</strong> per market maker per hour "
        f"across {mm['count']} of them.</div>"
    )

    # ---- per detector --------------------------------------------------
    parts.append("<h2>Per detector</h2>")
    rows = []
    for name in detectors:
        d = tuned["detectors"][name]
        r = raw["detectors"][name]
        rows.append([
            name, d["episodes"], d["alerts"], d["true_positives"], d["false_positives"],
            d["false_negatives"], d["misclassified"],
            f"{r['precision']:.3f} &rarr; {d['precision']:.3f}",
            f"{r['recall']:.3f} &rarr; {d['recall']:.3f}",
            f"{d['f1']:.3f}",
            f"{ev['operating_points'].get(name)}",
        ])
    parts.append(table(
        ["detector", "episodes", "alerts", "TP", "FP", "FN", "mislabelled",
         "precision (untuned&rarr;tuned)", "recall (untuned&rarr;tuned)", "F1", "threshold"],
        rows,
    ))
    parts.append(
        "<p>&ldquo;Mislabelled&rdquo; alerts overlap a real episode but call it by another "
        "detector's name -- most often a layering episode reported as spoofing, because a "
        "layer contains individually large orders. They are counted separately rather than "
        "as false positives: the analyst was pointed at real abuse.</p>"
    )

    # ---- curves --------------------------------------------------------
    parts.append("<h2>Operating points</h2>")
    series: dict[str, list[tuple[float, float, float]]] = {}
    marks: dict[str, tuple[float, float]] = {}
    mm_series: dict[str, list[tuple[float, float]]] = {}
    f1_series: dict[str, list[tuple[float, float]]] = {}
    for name in detectors:
        pts = sweep["detectors"][name]["points"]
        series[name] = [(p["recall"], p["precision"], p["threshold"]) for p in pts]
        mm_series[name] = [(p["threshold"], p["mm_fp_per_mm_hour"]) for p in pts]
        f1_series[name] = [(p["threshold"], p["f1"]) for p in pts]
        chosen = sweep["detectors"][name]["operating_point"].get("point")
        if chosen:
            marks[name] = (chosen["recall"], chosen["precision"])
    parts.append("<div class='charts'>")
    parts.append("<div class='chart'><h4>Precision against recall</h4>"
                 + svg.pr_curve(series, marks=marks)
                 + "<p class='sub' style='margin:.3rem .3rem 0'>Rings mark the chosen "
                   "operating point.</p></div>")
    parts.append("<div class='chart'><h4>F1 against threshold</h4>"
                 + svg.line_chart(f1_series, "threshold", "F1", ylim=(0.0, 1.0)) + "</div>")
    parts.append(
        "<div class='chart'><h4>Market-maker false positives per MM-hour</h4>"
        + svg.line_chart(mm_series, "threshold", "FP / MM / hour",
                         hline=sweep["mm_budget_per_mm_hour"])
        + "<p class='sub' style='margin:.3rem .3rem 0'>Dashed line is the configured "
          "budget; the operating point is the highest-F1 threshold beneath it.</p></div>"
    )
    parts.append("<div class='chart'><h4>Alerts per hour, tuned</h4>"
                 + svg.bar_chart(detectors,
                                 [tuned["detectors"][n]["alerts_per_hour"] for n in detectors],
                                 "alerts / hour") + "</div>")
    parts.append("</div>")

    # ---- latency -------------------------------------------------------
    parts.append("<h2>Detection latency</h2>")
    parts.append(
        f"<p>Measured from the first event of an episode to the timestamp at which the "
        f"detector could have raised the alert. The engine scans every "
        f"<code>{ev['engine']['scan_interval_ms']:g} ms</code> of tape time, which is a "
        f"floor on all of these. Detectors whose evidence only exists after the fact carry "
        f"their own additional floor: momentum ignition cannot conclude anything until the "
        f"reversal window has closed.</p>"
    )
    parts.append(table(
        ["detector", "n", "min", "p50", "p90", "p99", "max"],
        [
            [
                name, tuned["detectors"][name]["latency_ms"]["n"],
                f"{tuned['detectors'][name]['latency_ms']['min']:.0f} ms",
                f"{tuned['detectors'][name]['latency_ms']['p50']:.0f} ms",
                f"{tuned['detectors'][name]['latency_ms']['p90']:.0f} ms",
                f"{tuned['detectors'][name]['latency_ms']['p99']:.0f} ms",
                f"{tuned['detectors'][name]['latency_ms']['max']:.0f} ms",
            ]
            for name in detectors
        ],
    ))

    # ---- recall by intensity -------------------------------------------
    parts.append("<h2>Recall by how blatant the abuse was</h2>")
    parts.append(
        "<p>Every injected episode carries an intensity in [0, 1] controlling order size, "
        "how fast the manipulator pulls, and how much of the move it takes. A single recall "
        "number averages over that and hides where the detector's floor is.</p>"
    )
    rows = []
    for name in detectors:
        by = tuned["detectors"][name]["recall_by_intensity"]
        row = [name]
        for bucket in ("low", "medium", "high"):
            b = by.get(bucket)
            row.append(f"{b['recall']:.2f} ({b['detected']}/{b['episodes']})" if b else "--")
        rows.append(row)
    parts.append(table(["detector", "low (<0.5)", "medium (0.5-0.75)", "high (>0.75)"], rows))

    # ---- workload ------------------------------------------------------
    w = tuned["workload"]
    parts.append("<h2>What this costs a desk</h2>")
    parts.append(table(
        ["", "untuned", "tuned"],
        [
            ["alerts", raw["overall"]["alerts"], o["alerts"]],
            ["alerts per hour", f"{raw['overall']['alerts_per_hour']:.1f}",
             f"{o['alerts_per_hour']:.1f}"],
            ["analyst-hours per market hour",
             f"{raw['workload']['analyst_hours_per_market_hour']:.2f}",
             f"{w['analyst_hours_per_market_hour']:.2f}"],
            ["precision", f"{raw['overall']['precision']:.3f}", f"{o['precision']:.3f}"],
            ["recall", f"{raw['overall']['recall']:.3f}", f"{o['recall']:.3f}"],
        ],
    ))
    parts.append(
        f"<p>At {w['triage_minutes_per_alert']:g} minutes per alert, the tuned configuration "
        f"needs {w['analyst_hours_per_market_hour']:.2f} analyst-hours for every hour of "
        f"market. The untuned configuration needs "
        f"{raw['workload']['analyst_hours_per_market_hour']:.2f}. That ratio, rather than "
        f"the precision figure, is what the threshold sweep is for.</p>"
    )
    parts.append(
        f"<p>{ev['counts']['suppressed_by_cooldown']:,} further alerts were suppressed by "
        f"per-participant cooldowns inside the engine and never reached this evaluation. "
        f"They are counted rather than discarded because alert volume is the metric they "
        f"would otherwise quietly improve.</p>"
    )

    # ---- ablation ------------------------------------------------------
    parts.append("<h2>Which signals earn their weight</h2>")
    parts.append(
        "<p>Each row zeroes one signal's weight and re-scores the alerts the engine already "
        "emitted, at the same threshold. The hard gates are not ablated -- an alert that "
        "never fired cannot be recovered by editing a weight -- so this measures each "
        "signal's contribution to <em>ranking</em>. Zeroing a weight also renormalises the "
        "rest, so recall can rise.</p>"
    )
    for name in detectors:
        ab = ev["ablation"].get(name) or {}
        base = ab.get("baseline")
        if not base or not ab.get("signals"):
            continue
        parts.append(f"<h3>{esc(name)} &middot; threshold {ab['threshold']}</h3>")
        rows = [["(all signals)", "--", base["alerts"], f"{base['precision']:.3f}",
                 f"{base['recall']:.3f}", f"{base['f1']:.3f}", "--"]]
        for signal, v in sorted(ab["signals"].items(), key=lambda kv: kv[1]["delta_f1"]):
            rows.append([
                f"without {signal}", f"{v['weight']:.2f}", v["alerts"],
                f"{v['precision']:.3f}", f"{v['recall']:.3f}", f"{v['f1']:.3f}",
                delta(v["delta_f1"]),
            ])
        if ab.get("without_penalty"):
            v = ab["without_penalty"]
            rows.append([
                "without the market-maker penalty", "--", v["alerts"],
                f"{v['precision']:.3f}", f"{v['recall']:.3f}", f"{v['f1']:.3f}",
                delta(v["delta_f1"]),
            ])
        parts.append(table(
            ["configuration", "weight", "alerts", "precision", "recall", "F1", "&Delta;F1"],
            rows,
        ))

    # ---- feed ----------------------------------------------------------
    feed = ev["feed"]
    book = ev["book"]
    parts.append("<h2>Feed integrity</h2>")
    parts.append(
        "<p>The generator damages the tape on purpose. These counters are how the run "
        "reports what it was working with; a clean feed produces zeroes in every row "
        "except the first.</p>"
    )
    parts.append(table(
        ["counter", "value", "meaning"],
        [
            ["events admitted", f"{feed.get('events_admitted', 0):,}", "reached the detectors"],
            ["sequence gaps", feed.get("sequence_gaps", 0),
             f"{feed.get('sequence_missing', 0)} events implied missing"],
            ["duplicate event ids", feed.get("duplicate_event_ids", 0), "dropped before the book"],
            ["timestamp/sequence conflicts", feed.get("sequence_backward", 0),
             "kept in timestamp order, window marked degraded"],
            ["reordered and recovered", feed.get("reordered_recovered", 0),
             "put back in order by the reorder buffer"],
            ["reordered and dropped", feed.get("reordered_dropped", 0),
             "arrived beyond the tolerance"],
            ["stale levels repaired", book.get("stale_levels_removed", 0),
             "crossed book caused by a lost message"],
            ["unknown cancels", book.get("unknown_cancel", 0), "cancel for an order never seen"],
            ["unknown makers", book.get("unknown_maker", 0), "trade against an order never seen"],
            ["alerts marked degraded", ev["counts"]["degraded_alerts"],
             "raised near feed damage; confidence reduced"],
        ],
    ))

    # ---- misses --------------------------------------------------------
    misses = ev["missed_episodes"]
    parts.append("<h2>What was missed</h2>")
    if not misses:
        parts.append("<p>Nothing. Every injected episode produced at least one correctly "
                     "typed alert at the chosen operating point.</p>")
    else:
        parts.append(
            f"<p>{len(misses)} of {ev['counts']['episodes']} episodes produced no correctly "
            f"typed alert. The &ldquo;other alerts&rdquo; column counts alerts that did "
            f"overlap the episode but named a different pattern.</p>"
        )
        parts.append(table(
            ["episode", "kind", "participant", "intensity", "duration", "other alerts"],
            [
                [m["episode_id"], m["kind"], m["participant"], f"{m['intensity']:.2f}",
                 f"{m['duration_ms']:,} ms", m["offtype_alerts"]]
                for m in misses[:40]
            ],
        ))

    # ---- provenance ----------------------------------------------------
    parts.append("<h2>Provenance</h2>")
    parts.append(table(
        ["", ""],
        [
            ["tape", f"{ev['generation']['events']:,} events, "
                     f"{ev['tape_seconds']:.0f} s, "
                     f"{ev['generation']['wall_seconds']:.1f} s to generate"],
            ["detection", f"{ev['engine']['events']:,} events in "
                          f"{ev['engine']['wall_seconds']:.2f} s "
                          f"({ev['engine']['events_per_second']:,.0f} events/s), "
                          f"{ev['engine']['scans']:,} scans"],
            ["evaluation", f"{ev['eval_wall_seconds']:.1f} s"],
            ["participants", ", ".join(f"{k} x{v}" for k, v in
                                       ev["generation"]["roster_counts"].items())],
            ["match rule", f"participant and symbol must agree; alert window must overlap "
                           f"the episode widened by "
                           f"-{ev['match_rule']['pre_tolerance_ms']} ms / "
                           f"+{ev['match_rule']['post_tolerance_ms']} ms"],
        ],
    ))

    parts.append(
        "<footer>Generated from eval.json. Every figure on this page is measured from the "
        "run named at the top; none are estimated, projected or carried over from another "
        "configuration.</footer>"
    )
    parts.append("</div>")
    return "".join(parts)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render a Tapewatch evaluation as HTML.")
    ap.add_argument("--run", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    with open(os.path.join(args.run, "eval.json"), "r", encoding="utf-8") as fh:
        ev = json.load(fh)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(render(ev))
    if not args.quiet:
        size = os.path.getsize(args.out)
        print(f"report: wrote {args.out} ({size / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
