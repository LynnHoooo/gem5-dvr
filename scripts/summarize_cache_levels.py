#!/usr/bin/env python3
"""Build a compact cache-level comparison from gem5 stats files."""

import csv
import os
import sys


CACHES = ("system.cpu.dcache", "system.l2", "system.l3")
ORIGINS = (
    ("demand", "architecturalDemand", "Accesses"),
    ("dvr_source", "dvrSource", "Issued"),
    ("dvr_dependent", "dvrDependent", "Issued"),
)


def read_stats(path):
    values = {}
    with open(path) as stream:
        for line in stream:
            columns = line.split()
            if len(columns) >= 2 and columns[0] not in values:
                values[columns[0]] = columns[1]
    return values


def main():
    if len(sys.argv) < 3:
        raise SystemExit(
            "usage: summarize_cache_levels.py OUTPUT.csv LABEL=RESULT_DIR ..."
        )
    output = sys.argv[1]
    rows = []
    for item in sys.argv[2:]:
        label, root = item.split("=", 1)
        stats = read_stats(os.path.join(root, "stats.txt"))
        committed = int(float(stats.get("system.cpu.committedInsts", 0)))
        for cache in CACHES:
            for origin, prefix, access_suffix in ORIGINS:
                accesses = int(float(stats.get(cache + "." + prefix + access_suffix, 0)))
                hits = int(float(stats.get(cache + "." + prefix + "Hits", 0)))
                misses = int(float(stats.get(cache + "." + prefix + "Misses", 0)))
                rows.append({
                    "mode": label,
                    "cache": cache,
                    "origin": origin,
                    "accesses": accesses,
                    "hits": hits,
                    "misses": misses,
                    "local_miss_rate": misses / accesses if accesses else 0.0,
                    "mpki": misses * 1000.0 / committed if committed else 0.0,
                })
    with open(output, "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        for row in rows:
            row["local_miss_rate"] = "{:.6f}".format(row["local_miss_rate"])
            row["mpki"] = "{:.6f}".format(row["mpki"])
            writer.writerow(row)


if __name__ == "__main__":
    main()
