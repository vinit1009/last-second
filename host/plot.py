#!/usr/bin/env python3
"""Draw a frozen Last Second window (bbtool dump CSV) as an SVG timeline.

    host/plot.py runs/heartbeat.csv docs/heartbeat.svg --title "..." [--log runs/serial.log --scene heartbeat]

Top strip: one tick per sample the recorder received, so gaps stand out. Below: how late each loop
tick woke up (log scale), with the trigger, the gap and the post-trigger window marked. Times are ms
relative to the trigger, as bbtool writes them.
"""
import argparse
import csv
import math
import re
from html import escape

W, H = 1200, 520
L, R, TOP = 90, 40, 70
STRIP_Y, STRIP_H = TOP + 10, 40
PLOT_Y, PLOT_H = TOP + 80, 300
INK, MUTED, GRID, RED, AMBER, BLUE = "#1d2327", "#5f6b73", "#e3e7ea", "#c0392b", "#e8a33d", "#2463a6"


def load(path):
    with open(path) as f:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(f)]


def scene_facts(log_path, scene):
    """The culprit's end (ms after the trigger) for this scene, from the serial log, if any."""
    if not log_path:
        return None
    text = open(log_path, errors="replace").read()
    start = text.find(f"===LS-SCENE {scene}===")
    if start < 0:
        return None
    nxt = text.find("===LS-SCENE", start + 10)
    part = text[start: nxt if nxt > 0 else len(text)]
    m = re.search(r"ended without a goodbye, ([0-9.]+) ms after the trigger", part)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("svg")
    ap.add_argument("--title", default="Last Second: the frozen window")
    ap.add_argument("--from-ms", type=float, default=-60)
    ap.add_argument("--to-ms", type=float, default=220)
    ap.add_argument("--log", help="serial.log, to mark when the culprit's process ended")
    ap.add_argument("--scene", default="heartbeat")
    ap.add_argument("--post-ms", type=float, default=200)
    a = ap.parse_args()

    rows = load(a.csv)
    t0, t1 = a.from_ms, a.to_ms
    view = [r for r in rows if t0 <= r["t_ms"] <= t1]
    x = lambda t: L + (t - t0) / (t1 - t0) * (W - L - R)
    lo, hi = 1.0, 50000.0
    y = lambda us: PLOT_Y + PLOT_H - (math.log10(max(lo, min(hi, us))) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * PLOT_H

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="Helvetica, Arial, sans-serif">',
           f'<rect width="{W}" height="{H}" fill="#ffffff"/>',
           f'<text x="{L}" y="36" font-size="22" font-weight="bold" fill="{INK}">{escape(a.title)}</text>']

    # post-trigger window, then frozen
    out.append(f'<rect x="{x(0):.1f}" y="{STRIP_Y - 6}" width="{x(min(a.post_ms, t1)) - x(0):.1f}" height="{PLOT_Y + PLOT_H - STRIP_Y + 6}" fill="#fbf1e1"/>')
    if a.post_ms < t1:
        out.append(f'<rect x="{x(a.post_ms):.1f}" y="{STRIP_Y - 6}" width="{x(t1) - x(a.post_ms):.1f}" height="{PLOT_Y + PLOT_H - STRIP_Y + 6}" fill="#eef1f3"/>')
        out.append(f'<text x="{x(a.post_ms) + 6:.1f}" y="{PLOT_Y + 18}" font-size="13" fill="{MUTED}">frozen</text>')
    out.append(f'<text x="{x(0) + 6:.1f}" y="{PLOT_Y + PLOT_H - 8}" font-size="13" fill="{MUTED}">still recording for {a.post_ms:g} ms after the trigger</text>')

    # the longest gap in view
    times = [r["t_ms"] for r in rows]
    gap = max(((b - p, p, b) for p, b in zip(times, times[1:]) if b >= t0 and p <= t1), default=None)
    if gap and gap[0] > 2 and gap[2] <= t1:
        g, p, b = gap
        out.append(f'<rect x="{x(p):.1f}" y="{STRIP_Y - 6}" width="{x(min(b, t1)) - x(p):.1f}" height="{STRIP_H + 12}" fill="#f6d4cf"/>')
        label = f"no samples for {g:.1f} ms" if b <= t1 else "no samples after this"
        out.append(f'<text x="{x(p):.1f}" y="{STRIP_Y - 12}" font-size="13" fill="{RED}">{label}</text>')

    # samples stop before the end of the view: the producer is gone
    last = rows[-1] if rows else None
    if last is not None and t0 <= last["t_ms"] < min(t1, a.post_ms) - 2:  # stopped before the freeze
        out.append(f'<rect x="{x(last["t_ms"]):.1f}" y="{STRIP_Y - 6}" width="{x(t1) - x(last["t_ms"]):.1f}" height="{STRIP_H + 12}" fill="#f6d4cf"/>')
        out.append(f'<text x="{x(last["t_ms"]) + 6:.1f}" y="{STRIP_Y + STRIP_H / 2 + 5}" font-size="13" fill="{RED}">no samples after tick {int(last["seq"])}</text>')

    # sample strip
    out.append(f'<text x="{L - 10}" y="{STRIP_Y + STRIP_H / 2 + 5}" font-size="13" fill="{MUTED}" text-anchor="end">samples</text>')
    for r in view:
        out.append(f'<line x1="{x(r["t_ms"]):.1f}" x2="{x(r["t_ms"]):.1f}" y1="{STRIP_Y}" y2="{STRIP_Y + STRIP_H}" stroke="{BLUE}" stroke-width="1"/>')

    # lateness axis and grid
    for us, lab in [(1, "1 us"), (10, "10 us"), (100, "100 us"), (1000, "1 ms"), (10000, "10 ms")]:
        out.append(f'<line x1="{L}" x2="{W - R}" y1="{y(us):.1f}" y2="{y(us):.1f}" stroke="{GRID}"/>')
        out.append(f'<text x="{L - 10}" y="{y(us) + 4:.1f}" font-size="12" fill="{MUTED}" text-anchor="end">{lab}</text>')
    out.append(f'<text x="22" y="{PLOT_Y + PLOT_H / 2}" font-size="13" fill="{MUTED}" transform="rotate(-90 22 {PLOT_Y + PLOT_H / 2})" text-anchor="middle">how late each tick woke up</text>')
    for r in view:
        out.append(f'<circle cx="{x(r["t_ms"]):.1f}" cy="{y(r["v3"]):.1f}" r="2.4" fill="{BLUE}"/>')

    # time axis
    step = 20 if (t1 - t0) <= 300 else 100
    t = math.ceil(t0 / step) * step
    while t <= t1:
        out.append(f'<line x1="{x(t):.1f}" x2="{x(t):.1f}" y1="{PLOT_Y + PLOT_H}" y2="{PLOT_Y + PLOT_H + 6}" stroke="{MUTED}"/>')
        out.append(f'<text x="{x(t):.1f}" y="{PLOT_Y + PLOT_H + 22}" font-size="12" fill="{MUTED}" text-anchor="middle">{t:g}</text>')
        t += step
    out.append(f'<text x="{(L + W - R) / 2}" y="{PLOT_Y + PLOT_H + 44}" font-size="13" fill="{MUTED}" text-anchor="middle">ms relative to the trigger</text>')

    # trigger and the culprit's end
    out.append(f'<line x1="{x(0):.1f}" x2="{x(0):.1f}" y1="{STRIP_Y - 6}" y2="{PLOT_Y + PLOT_H}" stroke="{RED}" stroke-width="2"/>')
    out.append(f'<text x="{x(0) + 6:.1f}" y="{PLOT_Y - 8}" font-size="13" font-weight="bold" fill="{RED}">trigger: missed heartbeat</text>')
    end = scene_facts(a.log, a.scene)
    if end is not None and t0 <= end <= t1:
        out.append(f'<line x1="{x(end):.1f}" x2="{x(end):.1f}" y1="{STRIP_Y - 6}" y2="{PLOT_Y + PLOT_H}" stroke="{AMBER}" stroke-width="2" stroke-dasharray="5 4"/>')
        out.append(f'<text x="{x(end) + 6:.1f}" y="{PLOT_Y + 36}" font-size="13" fill="{INK}">process ended without a goodbye (+{end:.1f} ms)</text>')

    burst = [r for r in view if r["v3"] > 500]
    if burst:
        bx = x(burst[0]["t_ms"])
        out.append(f'<text x="{bx + 8:.1f}" y="{y(burst[0]["v3"]) + 4:.1f}" font-size="13" fill="{INK}">{len(burst)} late ticks run back to back</text>')
    out.append("</svg>")
    with open(a.svg, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{a.svg}: {len(view)} samples from {t0:g} to {t1:g} ms")


if __name__ == "__main__":
    main()
