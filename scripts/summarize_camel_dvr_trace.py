#!/usr/bin/env python3
import csv
import json
import os
import sys
import glob
from collections import Counter, defaultdict


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_camel_dvr_trace.py TRACE_DIR")
    root = sys.argv[1]
    dep = os.path.join(root, "dependency_chain.csv")
    by_trigger = defaultdict(Counter)
    kind_counts = Counter()
    flr_samples = defaultdict(list)
    stages_by_pc = defaultdict(Counter)
    with open(dep, newline="") as stream:
        for row in csv.DictReader(stream):
            trigger = row["trigger_pc"]
            pc = row["pc"]
            kind = row["kind"]
            by_trigger[trigger][pc] += 1
            kind_counts[kind] += 1
            stages_by_pc[pc][kind] += 1
            if trigger not in ("0", "0x0"):
                stages_by_pc[trigger]["trigger_events"] += 1
            if kind in ("flr", "nested_flr") and len(flr_samples[trigger]) < 5:
                flr_samples[trigger].append({
                    "pc": pc,
                    "address": row["address"],
                })

    triggers = []
    for trigger, pcs in sorted(
        by_trigger.items(), key=lambda item: sum(item[1].values()), reverse=True
    )[:20]:
        triggers.append({
            "trigger_pc": trigger,
            "events": sum(pcs.values()),
            "top_chain_pcs": [
                {"pc": pc, "count": count}
                for pc, count in pcs.most_common(16)
            ],
            "flr_samples": flr_samples.get(trigger, []),
        })

    summary = {
        "workload": "camel",
        "dependency_event_counts": dict(kind_counts),
        "dominant_triggers": triggers,
        "expected_pointer_chain": {
            "description": "array2[x] -> *array2[x]",
            "expected_trigger_pc": "0x1039a for the validated MAX_KEY=65536 ELF",
            "expected_flr_pc": "0x103a0 for the validated MAX_KEY=65536 ELF",
        },
    }
    with open(os.path.join(root, "dependency_summary.json"), "w") as stream:
        json.dump(summary, stream, indent=2)
        stream.write("\n")

    with open(os.path.join(root, "dependency_summary.csv"), "w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["trigger_pc", "events", "top_pcs", "flr_samples"])
        for item in triggers:
            writer.writerow([
                item["trigger_pc"],
                item["events"],
                " ".join(x["pc"] for x in item["top_chain_pcs"]),
                " ".join(x["pc"] for x in item["flr_samples"]),
            ])

    cache_by_pc = defaultdict(Counter)
    for path in glob.glob(os.path.join(root, "cache_pc_*.csv")):
        with open(path, newline="") as stream:
            for row in csv.DictReader(stream):
                pc = row["pc"]
                cache = row["cache"]
                if cache.endswith("dcache"):
                    for field in (
                        "demand_hits", "demand_misses", "dvr_source_hits",
                        "dvr_source_misses", "dvr_dependent_hits",
                        "dvr_dependent_misses",
                    ):
                        cache_by_pc[pc][field] += int(row[field])
                elif cache.endswith(".l2"):
                    cache_by_pc[pc]["l2_demand_misses"] += int(row["demand_misses"])
                elif cache.endswith(".l3"):
                    cache_by_pc[pc]["l3_demand_misses"] += int(row["demand_misses"])

    fields = [
        "pc", "demand_hits", "demand_misses", "dvr_source_hits",
        "dvr_source_misses", "dvr_dependent_hits", "dvr_dependent_misses",
        "l2_demand_misses", "l3_demand_misses",
        "trigger_events", "tainted", "flr", "nested_tainted", "nested_flr",
        "relation_trained", "replay_target", "alternate_replay_target",
        "translation_fault",
    ]
    all_pcs = set(cache_by_pc) | set(stages_by_pc)
    with open(os.path.join(root, "pc_pipeline_summary.csv"), "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for pc in sorted(
            all_pcs,
            key=lambda value: (
                -cache_by_pc[value]["demand_misses"],
                -stages_by_pc[value]["replay_target"],
                int(value, 0),
            ),
        ):
            row = {"pc": pc}
            row.update({field: cache_by_pc[pc][field] for field in fields[1:9]})
            row.update({field: stages_by_pc[pc][field] for field in fields[9:]})
            writer.writerow(row)
    print("CAMEL_DVR_TRACE_SUMMARY_WRITTEN", root)


if __name__ == "__main__":
    main()
