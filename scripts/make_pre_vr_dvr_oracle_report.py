#!/usr/bin/env python3
"""Summarize the matched PRE/VR/DVR/oracle experiments."""

import csv
import os


ROOT = os.environ.get("EXPERIMENT_ROOT",
    "/home/lynnhoo/dvr-repro/results/pre-vr-dvr-oracle")
MODES = ["Baseline", "PRE", "VR", "DVR", "Oracle"]
CASES = {
    "Camel": "camel",
    "BFS (Kron scale 8)": "bfs",
}


def read_stats(path):
    values = {}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith("system.cpu."):
                values[fields[0][len("system.cpu."):]] = fields[1]
    return values


def number(values, key):
    try:
        return float(values.get(key, "0"))
    except ValueError:
        return 0.0


def main():
    rows = []
    for label, directory in CASES.items():
        stats = {}
        for mode in MODES:
            dirname = "baseline" if mode == "Baseline" else mode
            if mode == "Oracle":
                dirname = "Oracle3"
            stats[mode] = read_stats(os.path.join(ROOT, directory,
                                                  dirname, "stats.txt"))
        baseline = number(stats["Baseline"], "ipc")
        for mode in MODES:
            values = stats[mode]
            rows.append({
                "workload": label,
                "mode": mode,
                "ipc": f"{number(values, 'ipc'):.6f}",
                "normalized_ipc": f"{number(values, 'ipc') / baseline:.6f}",
                "cycles": f"{number(values, 'numCycles'):.0f}",
                "committed_insts": f"{number(values, 'committedInsts'):.0f}",
                "oracle_generated": f"{number(values, 'oraclePrefetchesGenerated'):.0f}",
                "oracle_issued": f"{number(values, 'oraclePrefetchesIssued'):.0f}",
                "oracle_completed": f"{number(values, 'oraclePrefetchesCompleted'):.0f}",
                "oracle_demand_covered": f"{number(values, 'oracleDemandCovered'):.0f}",
                "dvr_dependent_issued": f"{number(values, 'dvrDependentPrefetchesIssued'):.0f}",
                "dvr_vector_programs": f"{number(values, 'dvrVectorProgramsBuilt'):.0f}",
            })

    csv_path = os.path.join(ROOT, "comparison.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    width, height = 1040, 600
    left, top, right, bottom = 90, 60, 30, 120
    plot_w, plot_h = width - left - right, height - top - bottom
    max_value = max(1.55, max(float(row["normalized_ipc"]) for row in rows) * 1.08)
    colors = ["#64748b", "#be123c", "#2563eb", "#0f766e", "#d97706"]
    group_width = 360
    bar_width = 40
    svg = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="white"/>',
        f'<text x="{left}" y="32" font-family="sans-serif" font-size="21" font-weight="600">PRE / VR / DVR / Oracle: IPC normalized to baseline OoO</text>',
        f'<text transform="translate(20 {top + plot_h // 2}) rotate(-90)" font-family="sans-serif" font-size="14">normalized IPC</text>',
    ]
    for tick in range(6):
        value = max_value * tick / 5
        y = top + plot_h - plot_h * value / max_value
        svg.append(f'<line x1="{left}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}" stroke="#e2e8f0"/>')
        svg.append(f'<text x="{left-10}" y="{y+4:.1f}" text-anchor="end" font-family="sans-serif" font-size="12" fill="#475569">{value:.2f}</text>')
    for group_index, workload in enumerate(CASES):
        group = [row for row in rows if row["workload"] == workload]
        gx = left + 80 + group_index * group_width
        for index, row in enumerate(group):
            value = float(row["normalized_ipc"])
            x = gx + index * bar_width
            bar_h = plot_h * value / max_value
            y = top + plot_h - bar_h
            svg.append(f'<rect x="{x}" y="{y:.1f}" width="{bar_width-5}" height="{bar_h:.1f}" fill="{colors[index]}"/>')
            svg.append(f'<text x="{x + (bar_width-5)/2:.1f}" y="{y-6:.1f}" text-anchor="middle" font-family="sans-serif" font-size="11">{value:.3f}</text>')
        center = gx + len(MODES) * bar_width / 2
        svg.append(f'<text x="{center:.1f}" y="{height-58}" text-anchor="middle" font-family="sans-serif" font-size="14" font-weight="600">{workload}</text>')
    for index, mode in enumerate(MODES):
        x = left + index * 175
        svg.append(f'<rect x="{x}" y="{height-28}" width="12" height="12" fill="{colors[index]}"/>')
        svg.append(f'<text x="{x+18}" y="{height-17}" font-family="sans-serif" font-size="11">{mode}</text>')
    svg.append('</svg>\n')
    svg_path = os.path.join(ROOT, "comparison.svg")
    with open(svg_path, "w", encoding="utf-8") as stream:
        stream.write("\n".join(svg))
    print(csv_path)
    print(svg_path)


if __name__ == "__main__":
    main()
