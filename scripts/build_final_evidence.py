#!/usr/bin/env python3
"""Build the human-readable evidence bundle for one DVR ablation run."""

import argparse
import csv
import html
import math
import os
import struct
import subprocess
import zlib
from collections import defaultdict
from pathlib import Path


MODES = ["Baseline", "VR", "Offload", "Discovery", "Multiple", "Oracle"]
COLORS = {
    "Baseline": "#6b7280",
    "VR": "#2563eb",
    "Offload": "#059669",
    "Discovery": "#d97706",
    "Multiple": "#dc2626",
    "Oracle": "#7c3aed",
}


def read_csv(path: Path):
    with path.open(newline="") as fh:
        return list(csv.DictReader(fh))


def manifest_values(path: Path):
    values = {}
    if path.exists():
        for line in path.read_text().splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
    return values


def hmean(values):
    positive = [float(v) for v in values if float(v) > 0]
    return len(positive) / sum(1.0 / v for v in positive) if positive else 0.0


def write_configuration(run: Path, manifest: dict):
    rows = [
        ("git_sha", manifest.get("git_sha", ""), "manifest.txt"),
        ("gem5_binary", manifest.get("gem5", ""), "manifest.txt"),
        ("gem5_sha256", manifest.get("gem5_sha256", ""), "manifest.txt"),
        ("benchmark_root", manifest.get("benchmark_root", ""), "manifest.txt"),
        ("workloads", manifest.get("workloads", ""), "manifest.txt"),
        ("bfs_scale", manifest.get("bfs_scale", ""), "manifest.txt"),
        ("camel_max_key", manifest.get("camel_max_key", ""), "manifest.txt"),
        ("oracle_lookahead", manifest.get("oracle_lookahead", ""), "manifest.txt"),
        ("cpu_clock", "4GHz", "table1_se.py"),
        ("fetch_decode_rename_dispatch_issue_commit_width", "5", "table1_se.py"),
        ("rob_iq_lq_sq", "350/128/128/72", "table1_se.py"),
        ("cache_line_bytes", "64", "table1_se.py"),
        ("dvr_vector_chunk_model", "enabled for VR/Offload/Discovery/Multiple", "run script"),
        ("dvr_vector_element_bits", "64", "run script/config default"),
        ("dvr_helper_max_uops", "200", "table1_se.py default"),
        ("dvr_max_lanes", "128", "table1_se.py default"),
        ("ndm_threshold", "64", "table1_se.py default"),
        ("ndm_outer_invocation_cap", "16", "cpu implementation"),
        ("mode_mapping", "VR=vr; Offload=offload; Discovery=full; Multiple=nested; Oracle=trace", "run script"),
    ]
    with (run / "configuration.csv").open("w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(("setting", "value", "source"))
        writer.writerows(rows)


def make_svg(run: Path, performance):
    grouped = defaultdict(dict)
    for row in performance:
        grouped[row["workload"]][row["mode"]] = float(row["normalized_ipc"])
    workloads = sorted(grouped)
    hmean_values = {mode: hmean([grouped[w].get(mode, 0.0) for w in workloads]) for mode in MODES}
    groups = workloads + ["H-mean"]
    values = {w: (hmean_values if w == "H-mean" else grouped[w]) for w in groups}

    width, height = 1500, 820
    left, right, top, bottom = 90, 30, 70, 170
    plot_w, plot_h = width - left - right, height - top - bottom
    max_value = max(1.8, max((max(v.values()) for v in values.values()), default=1.0) * 1.15)
    group_w = plot_w / len(groups)
    bar_w = min(42, group_w / (len(MODES) + 1.5))
    baseline_y = top + plot_h
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:Arial,sans-serif;fill:#111827} .small{font-size:16px} .axis{font-size:18px} .title{font-size:28px;font-weight:700}</style>',
        '<text x="750" y="38" text-anchor="middle" class="title">DVR normalized IPC ablation (relative to Baseline)</text>',
        f'<line x1="{left}" y1="{baseline_y}" x2="{width-right}" y2="{baseline_y}" stroke="#374151" stroke-width="2"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{baseline_y}" stroke="#374151" stroke-width="2"/>',
    ]
    for tick in range(0, math.ceil(max_value * 10) + 1, 5):
        val = tick / 10.0
        y = baseline_y - val / max_value * plot_h
        parts.append(f'<line x1="{left}" y1="{y:.1f}" x2="{width-right}" y2="{y:.1f}" stroke="#e5e7eb"/>')
        parts.append(f'<text x="{left-12}" y="{y+6:.1f}" text-anchor="end" class="small">{val:.1f}</text>')
    for gi, group in enumerate(groups):
        center = left + (gi + 0.5) * group_w
        data = values[group]
        start = center - len(MODES) * bar_w / 2
        for mi, mode in enumerate(MODES):
            val = data.get(mode, 0.0)
            bh = val / max_value * plot_h
            x = start + mi * bar_w
            y = baseline_y - bh
            parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w-3:.1f}" height="{bh:.1f}" fill="{COLORS[mode]}"/>')
            parts.append(f'<text x="{x + (bar_w-3)/2:.1f}" y="{max(top+16, y-5):.1f}" text-anchor="middle" class="small">{val:.2f}</text>')
        parts.append(f'<text x="{center:.1f}" y="{baseline_y+32}" text-anchor="middle" class="axis">{html.escape(group)}</text>')
    parts.append(f'<text x="24" y="{top + plot_h/2}" transform="rotate(-90 24 {top + plot_h/2})" text-anchor="middle" class="axis">Normalized IPC</text>')
    legend_y = height - 78
    for i, mode in enumerate(MODES):
        x = left + i * 215
        parts.append(f'<rect x="{x}" y="{legend_y-15}" width="18" height="18" fill="{COLORS[mode]}"/>')
        parts.append(f'<text x="{x+26}" y="{legend_y}" class="small">{mode}</text>')
    parts.append('</svg>')
    svg = run / "figure8.svg"
    svg.write_text("\n".join(parts))
    html_path = run / "figure8.html"
    html_path.write_text(f'<html><body style="margin:0">{svg.read_text()}</body></html>')
    png = run / "figure8.png"
    try:
        subprocess.run([
            "firefox", "--headless", "--no-remote",
            f"--screenshot={png}", "--window-size=1500,820",
            html_path.as_uri(),
        ], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
    except (OSError, subprocess.SubprocessError):
        # SVG remains the lossless fallback when Firefox is unavailable.
        pass
    if not png.exists():
        write_png_chart(png, grouped, hmean_values)
    return grouped, hmean_values


def write_png_chart(path: Path, grouped, hmean_values):
    """Write a dependency-free raster fallback when no plotting package exists."""
    width, height = 1500, 820
    left, right, top, bottom = 90, 30, 70, 170
    plot_w, plot_h = width - left - right, height - top - bottom
    modes = MODES
    groups = sorted(grouped) + ["H-mean"]
    data = {w: (hmean_values if w == "H-mean" else grouped[w]) for w in groups}
    max_value = max(1.8, max((max(v.values()) for v in data.values()), default=1.0) * 1.15)
    pixels = bytearray([255, 255, 255] * width * height)

    def color(hex_color):
        return tuple(int(hex_color[i:i + 2], 16) for i in (1, 3, 5))

    def rect(x0, y0, x1, y1, rgb):
        x0, y0 = max(0, int(x0)), max(0, int(y0))
        x1, y1 = min(width, int(x1)), min(height, int(y1))
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(rgb) * (x1 - x0)
        for y in range(y0, y1):
            off = (y * width + x0) * 3
            pixels[off:off + len(row)] = row

    # Plot background, grid, and axes.
    rect(left, top, width - right, top + plot_h, (249, 250, 251))
    for tick in range(0, math.ceil(max_value * 10) + 1, 5):
        val = tick / 10.0
        y = top + plot_h - val / max_value * plot_h
        rect(left, y, width - right, y + 1, (229, 231, 235))
    rect(left, top, left + 2, top + plot_h, (55, 65, 81))
    rect(left, top + plot_h, width - right, top + plot_h + 2, (55, 65, 81))
    group_w = plot_w / len(groups)
    bar_w = min(42, group_w / (len(modes) + 1.5))
    for gi, group in enumerate(groups):
        center = left + (gi + 0.5) * group_w
        start = center - len(modes) * bar_w / 2
        for mi, mode in enumerate(modes):
            value = data[group].get(mode, 0.0)
            bh = value / max_value * plot_h
            x = start + mi * bar_w
            rect(x, top + plot_h - bh, x + bar_w - 3, top + plot_h, color(COLORS[mode]))

    raw = b"".join(b"\x00" + bytes(pixels[y * width * 3:(y + 1) * width * 3]) for y in range(height))
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def write_readme(run: Path, grouped, hmeans, resource_dir=None):
    lines = [
        "# DVR LeAP evidence bundle",
        "",
        "This directory contains one unified Camel/BFS ablation run using the LeAP sources under `/home/lynnhoo/gem-test/gem5-leap/leap-bench`.",
        "The bars are normalized to the Baseline IPC for the same workload and simulation configuration.",
        "",
        "## Modes",
        "",
        "`Baseline`, `VR`, `Offload`, `Discovery` (`--dvr-mode=full`), `Multiple` (`--dvr-mode=nested`), and `Oracle` (baseline workload trace with lookahead 32).",
        "",
        "## Observed normalized IPC",
        "",
        "| Workload | Baseline | VR | Offload | Discovery | Multiple | Oracle |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for workload in sorted(grouped):
        lines.append("| " + workload + " | " + " | ".join(f"{grouped[workload].get(m, 0):.4f}" for m in MODES) + " |")
    lines.append("| H-mean | " + " | ".join(f"{hmeans.get(m, 0):.4f}" for m in MODES) + " |")
    lines += [
        "",
        "## Interpretation",
        "",
        "The CSV files are the authoritative evidence. `correctness.csv` marks a row `pass` only when committed instructions match baseline, translation faults are zero, and helper/dependent issued requests equal completed requests. Rows marked `observe` are intentionally not treated as paper-level passes.",
        "",
        "In this run, Offload improves simulated IPC but has translation faults on both workloads; this is an implementation issue, not a valid final claim. Discovery and Multiple produce helper/dependent activity, but Multiple does not yet improve IPC on these inputs. BFS program verification reports PASS in each stdout log, while some DVR rows have committed-count mismatches and therefore remain `observe`.",
        "",
        "`mechanism.csv` records loop-bound discovery, vector programs, replay/dependent targets, NDM flattening, alternate-path and reconvergence counters. `configuration.csv` records the shared simulation configuration. `figure8.svg` is the lossless chart source and `figure8.png` is the raster chart (generated with a dependency-free fallback when needed).",
        "",
        "Camel resource sensitivity is in `" + (str(resource_dir / "resource_sensitivity.csv") if resource_dir else "(not supplied)") + "`; it covers MaxUops, vector FU, element width, and lane-cap sweeps. VIR-copy and helper-frontend-width sweeps are marked `not_exposed` because the current configuration has no corresponding CLI controls.",
        "",
        "This package is a reproducible calibration result for Camel and BFS, not a claim that all Figure 8 trends from the paper have been reproduced.",
    ]
    (run / "README.md").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--resource-dir", type=Path, default=None)
    args = parser.parse_args()
    run = args.run_dir.resolve()
    performance = read_csv(run / "performance.csv")
    manifest = manifest_values(run / "manifest.txt")
    write_configuration(run, manifest)
    grouped, hmeans = make_svg(run, performance)
    resource_dir = args.resource_dir.resolve() if args.resource_dir else None
    write_readme(run, grouped, hmeans, resource_dir)
    print(run / "configuration.csv")
    print(run / "figure8.svg")
    print(run / "figure8.png")
    print(run / "README.md")


if __name__ == "__main__":
    main()
