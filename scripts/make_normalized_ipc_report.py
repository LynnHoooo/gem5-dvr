#!/usr/bin/env python3
"""Collect the two-workload DVR ablation and render a dependency-free SVG."""

import csv
import os
import re


ROOT = "/home/lynnhoo/dvr-repro/results/dvr-camel-bfs-figure8"
MODES = ["Baseline", "Vector Runahead", "Offload", "Discovery", "Nested"]
CASES = {
    "Camel": ["smoke-camel-baseline", "camel-vr", "camel-offload",
               "camel-discovery", "camel-nested"],
    "BFS (Kron scale 8)": ["bfs-scale8-baseline", "bfs-scale8-vr",
                           "bfs-scale8-offload", "bfs-scale8-discovery",
                           "bfs-scale8-nested"],
}


def stats(case):
    values = {}
    path = os.path.join(ROOT, case, "stats.txt")
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith("system.cpu."):
                values[fields[0][len("system.cpu."):]] = fields[1]
    return values


def num(values, key):
    value = values.get(key, "0")
    try:
        return float(value)
    except ValueError:
        return 0.0


def write_csv(rows):
    path = os.path.join(ROOT, "normalized_ipc.csv")
    fields = ["workload", "mode", "ipc", "normalized_ipc", "cycles",
              "committed_insts", "vector_programs_built", "replay_attempts",
              "replay_targets_generated"]
    with open(path, "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    return path


def write_svg(rows):
    grouped = {}
    for row in rows:
        grouped.setdefault(row["workload"], {})[row["mode"]] = float(
            row["normalized_ipc"])
    width, height = 980, 580
    left, right, top, bottom = 90, 30, 55, 110
    plot_w, plot_h = width - left - right, height - top - bottom
    max_value = max(1.05, max(max(v.values()) for v in grouped.values()) * 1.08)
    colors = ["#64748b", "#2563eb", "#0f766e", "#d97706", "#9333ea"]
    bar_w = 34
    group_gap = 80
    group_w = len(MODES) * bar_w + group_gap
    x0 = left + 100
    svg = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d">' % (width, height, width, height),
        '<rect width="100%%" height="100%%" fill="#ffffff"/>',
        '<text x="%d" y="30" font-family="sans-serif" font-size="20" '
        'font-weight="600">DVR performance normalized to baseline OoO</text>' % left,
        '<text transform="translate(20 %d) rotate(-90)" font-family="sans-serif" '
        'font-size="14">IPC normalized to baseline OoO</text>' % (top + plot_h // 2),
    ]
    for tick in range(0, 6):
        value = max_value * tick / 5.0
        y = top + plot_h - plot_h * value / max_value
        svg.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" '
                   'stroke="#e2e8f0"/>' % (left, y, width - right, y))
        svg.append('<text x="%d" y="%.1f" text-anchor="end" '
                   'font-family="sans-serif" font-size="12" fill="#475569">%.2f</text>'
                   % (left - 10, y + 4, value))
    for group_index, (workload, values) in enumerate(grouped.items()):
        gx = x0 + group_index * group_w
        center = gx + len(MODES) * bar_w / 2
        svg.append('<text x="%.1f" y="%d" text-anchor="middle" '
                   'font-family="sans-serif" font-size="14" font-weight="600">%s</text>'
                   % (center, height - 58, workload))
        for mode_index, mode in enumerate(MODES):
            value = values[mode]
            x = gx + mode_index * bar_w
            bar_h = plot_h * value / max_value
            y = top + plot_h - bar_h
            svg.append('<rect x="%d" y="%.1f" width="%d" height="%.1f" '
                       'fill="%s"/>' % (x, y, bar_w - 4, bar_h, colors[mode_index]))
            svg.append('<text x="%.1f" y="%.1f" text-anchor="middle" '
                       'font-family="sans-serif" font-size="11" fill="#0f172a">%.3f</text>'
                       % (x + (bar_w - 4) / 2, y - 5, value))
    legend_y = height - 20
    for index, mode in enumerate(MODES):
        x = left + index * 175
        svg.append('<rect x="%d" y="%d" width="12" height="12" fill="%s"/>'
                   % (x, legend_y - 10, colors[index]))
        svg.append('<text x="%d" y="%d" font-family="sans-serif" font-size="11">%s</text>'
                   % (x + 18, legend_y, mode))
    svg.append("</svg>\n")
    path = os.path.join(ROOT, "normalized_ipc.svg")
    with open(path, "w", encoding="utf-8") as stream:
        stream.write("\n".join(svg))
    return path


def main():
    rows = []
    for workload, cases in CASES.items():
        parsed = [stats(case) for case in cases]
        baseline = num(parsed[0], "ipc")
        if baseline <= 0:
            raise SystemExit("missing baseline IPC for %s" % workload)
        for mode, values in zip(MODES, parsed):
            rows.append({
                "workload": workload,
                "mode": mode,
                "ipc": "%.6f" % num(values, "ipc"),
                "normalized_ipc": "%.6f" % (num(values, "ipc") / baseline),
                "cycles": "%.0f" % num(values, "numCycles"),
                "committed_insts": "%.0f" % num(values, "committedInsts"),
                "vector_programs_built": "%.0f" % num(values, "dvrVectorProgramsBuilt"),
                "replay_attempts": "%.0f" % num(values, "dvrReplayAttempts"),
                "replay_targets_generated": "%.0f" % num(values, "dvrReplayTargetsGenerated"),
            })
    print(write_csv(rows))
    print(write_svg(rows))


if __name__ == "__main__":
    main()
