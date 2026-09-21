"""Hand-rolled SVG charts.

Matplotlib would be three lines each. It would also be a dependency, and the
report is four chart types -- a line chart, a scatter, a bar chart and a
histogram -- all of which are twenty lines of string formatting. The whole
Python side of Tapewatch runs on a bare interpreter with no pip, and these
charts are not worth giving that up for.

Charts are inline SVG so the report is one self-contained HTML file that can
be attached to an email and still work.
"""

from __future__ import annotations

from typing import Iterable, Sequence

PALETTE = ["#2f6f4f", "#8a4b2a", "#3a5a8c", "#7a3a6a", "#6a6a2a", "#2a6a6a"]


def _esc(text: str) -> str:
    return (
        str(text)
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


class Axes:
    """A linear 2-D mapping from data space to SVG pixels."""

    def __init__(self, width: int, height: int, xlim: tuple[float, float],
                 ylim: tuple[float, float], pad_left: int = 52, pad_bottom: int = 34,
                 pad_top: int = 14, pad_right: int = 14) -> None:
        self.w, self.h = width, height
        self.x0, self.x1 = xlim
        self.y0, self.y1 = ylim
        if self.x1 <= self.x0:
            self.x1 = self.x0 + 1.0
        if self.y1 <= self.y0:
            self.y1 = self.y0 + 1.0
        self.pl, self.pb, self.pt, self.pr = pad_left, pad_bottom, pad_top, pad_right

    @property
    def plot_w(self) -> float:
        return self.w - self.pl - self.pr

    @property
    def plot_h(self) -> float:
        return self.h - self.pt - self.pb

    def px(self, x: float) -> float:
        return self.pl + (x - self.x0) / (self.x1 - self.x0) * self.plot_w

    def py(self, y: float) -> float:
        return self.pt + (1.0 - (y - self.y0) / (self.y1 - self.y0)) * self.plot_h


def _frame(ax: Axes, xlabel: str, ylabel: str, xticks: Sequence[float],
           yticks: Sequence[float], xfmt: str = "{:.2f}", yfmt: str = "{:.2f}") -> list[str]:
    out = [
        f'<rect x="{ax.pl}" y="{ax.pt}" width="{ax.plot_w:.1f}" height="{ax.plot_h:.1f}" '
        f'fill="#ffffff" stroke="#d0cec8"/>'
    ]
    for t in yticks:
        y = ax.py(t)
        out.append(
            f'<line x1="{ax.pl}" y1="{y:.1f}" x2="{ax.pl + ax.plot_w:.1f}" y2="{y:.1f}" '
            f'stroke="#eeece6"/>'
        )
        out.append(
            f'<text x="{ax.pl - 6}" y="{y + 3.5:.1f}" text-anchor="end" '
            f'font-size="10" fill="#6b6862">{_esc(yfmt.format(t))}</text>'
        )
    for t in xticks:
        x = ax.px(t)
        out.append(
            f'<line x1="{x:.1f}" y1="{ax.pt}" x2="{x:.1f}" y2="{ax.pt + ax.plot_h:.1f}" '
            f'stroke="#eeece6"/>'
        )
        out.append(
            f'<text x="{x:.1f}" y="{ax.pt + ax.plot_h + 14:.1f}" text-anchor="middle" '
            f'font-size="10" fill="#6b6862">{_esc(xfmt.format(t))}</text>'
        )
    out.append(
        f'<text x="{ax.pl + ax.plot_w / 2:.1f}" y="{ax.h - 4}" text-anchor="middle" '
        f'font-size="11" fill="#45423d">{_esc(xlabel)}</text>'
    )
    out.append(
        f'<text x="12" y="{ax.pt + ax.plot_h / 2:.1f}" text-anchor="middle" font-size="11" '
        f'fill="#45423d" transform="rotate(-90 12 {ax.pt + ax.plot_h / 2:.1f})">'
        f"{_esc(ylabel)}</text>"
    )
    return out


def ticks(lo: float, hi: float, n: int = 5) -> list[float]:
    if hi <= lo:
        return [lo]
    step = (hi - lo) / n
    return [round(lo + i * step, 6) for i in range(n + 1)]


def pr_curve(series: dict[str, list[tuple[float, float, float]]], width: int = 460,
             height: int = 300, marks: dict[str, tuple[float, float]] | None = None) -> str:
    """Precision against recall. Each series is [(recall, precision, threshold)]."""
    ax = Axes(width, height, (0.0, 1.0), (0.0, 1.0))
    parts = [f'<svg viewBox="0 0 {width} {height}" width="100%" '
             f'xmlns="http://www.w3.org/2000/svg" role="img">']
    parts += _frame(ax, "recall", "precision", ticks(0, 1), ticks(0, 1), "{:.1f}", "{:.1f}")

    for i, (name, points) in enumerate(sorted(series.items())):
        colour = PALETTE[i % len(PALETTE)]
        ordered = sorted(points, key=lambda p: p[0])
        if not ordered:
            continue
        path = " ".join(
            f"{'M' if k == 0 else 'L'}{ax.px(r):.1f},{ax.py(p):.1f}"
            for k, (r, p, _) in enumerate(ordered)
        )
        parts.append(f'<path d="{path}" fill="none" stroke="{colour}" stroke-width="1.8"/>')
        for r, p, _t in ordered:
            parts.append(f'<circle cx="{ax.px(r):.1f}" cy="{ax.py(p):.1f}" r="1.8" '
                         f'fill="{colour}" opacity="0.65"/>')
        if marks and name in marks:
            r, p = marks[name]
            parts.append(
                f'<circle cx="{ax.px(r):.1f}" cy="{ax.py(p):.1f}" r="5" fill="none" '
                f'stroke="{colour}" stroke-width="2"/>'
            )
        y = ax.pt + 12 + i * 14
        parts.append(f'<rect x="{ax.pl + 10}" y="{y - 8}" width="10" height="3" fill="{colour}"/>')
        parts.append(f'<text x="{ax.pl + 25}" y="{y - 3}" font-size="10" fill="#45423d">'
                     f"{_esc(name)}</text>")
    parts.append("</svg>")
    return "".join(parts)


def line_chart(series: dict[str, list[tuple[float, float]]], xlabel: str, ylabel: str,
               width: int = 460, height: int = 300, ylim: tuple[float, float] | None = None,
               hline: float | None = None) -> str:
    xs = [x for pts in series.values() for x, _ in pts]
    ys = [y for pts in series.values() for _, y in pts]
    if not xs:
        return ""
    y_lo, y_hi = ylim if ylim else (0.0, max(ys) * 1.1 or 1.0)
    ax = Axes(width, height, (min(xs), max(xs)), (y_lo, y_hi))
    parts = [f'<svg viewBox="0 0 {width} {height}" width="100%" '
             f'xmlns="http://www.w3.org/2000/svg" role="img">']
    parts += _frame(ax, xlabel, ylabel, ticks(min(xs), max(xs)), ticks(y_lo, y_hi),
                    "{:.2f}", "{:.1f}")
    if hline is not None and y_lo <= hline <= y_hi:
        parts.append(
            f'<line x1="{ax.pl}" y1="{ax.py(hline):.1f}" x2="{ax.pl + ax.plot_w:.1f}" '
            f'y2="{ax.py(hline):.1f}" stroke="#b03030" stroke-width="1.2" '
            f'stroke-dasharray="4 3"/>'
        )
    for i, (name, pts) in enumerate(sorted(series.items())):
        colour = PALETTE[i % len(PALETTE)]
        ordered = sorted(pts)
        path = " ".join(
            f"{'M' if k == 0 else 'L'}{ax.px(x):.1f},{ax.py(min(max(y, y_lo), y_hi)):.1f}"
            for k, (x, y) in enumerate(ordered)
        )
        parts.append(f'<path d="{path}" fill="none" stroke="{colour}" stroke-width="1.8"/>')
        y = ax.pt + 12 + i * 14
        parts.append(f'<rect x="{ax.pl + 10}" y="{y - 8}" width="10" height="3" fill="{colour}"/>')
        parts.append(f'<text x="{ax.pl + 25}" y="{y - 3}" font-size="10" fill="#45423d">'
                     f"{_esc(name)}</text>")
    parts.append("</svg>")
    return "".join(parts)


def bar_chart(labels: Sequence[str], values: Sequence[float], ylabel: str,
              width: int = 460, height: int = 260, colours: Iterable[str] | None = None) -> str:
    if not labels:
        return ""
    top = max(values) * 1.15 or 1.0
    ax = Axes(width, height, (0.0, float(len(labels))), (0.0, top), pad_bottom=46)
    parts = [f'<svg viewBox="0 0 {width} {height}" width="100%" '
             f'xmlns="http://www.w3.org/2000/svg" role="img">']
    parts += _frame(ax, "", ylabel, [], ticks(0, top), "{:.0f}", "{:.1f}")
    cols = list(colours) if colours else [PALETTE[i % len(PALETTE)] for i in range(len(labels))]
    slot = ax.plot_w / len(labels)
    for i, (label, value) in enumerate(zip(labels, values)):
        x = ax.pl + i * slot + slot * 0.18
        w = slot * 0.64
        y = ax.py(max(0.0, min(value, top)))
        parts.append(
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" '
            f'height="{ax.pt + ax.plot_h - y:.1f}" fill="{cols[i]}"/>'
        )
        parts.append(
            f'<text x="{x + w / 2:.1f}" y="{y - 4:.1f}" text-anchor="middle" font-size="10" '
            f'fill="#45423d">{_esc(f"{value:g}")}</text>'
        )
        parts.append(
            f'<text x="{x + w / 2:.1f}" y="{ax.pt + ax.plot_h + 14:.1f}" text-anchor="middle" '
            f'font-size="9.5" fill="#6b6862">{_esc(label)}</text>'
        )
    parts.append("</svg>")
    return "".join(parts)
