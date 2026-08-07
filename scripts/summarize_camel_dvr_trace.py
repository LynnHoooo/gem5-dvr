#!/usr/bin/env python3
import csv
import json
import os
import sys
from collections import Counter, defaultdict


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: summarize_camel_dvr_trace.py TRACE_DIR")
    root = sys.argv[1]
    dep = os.path.join(root, "dependency_chain.csv")
    by_trigger = defaultdict(Counter)
    kind_counts = Counter()
    flr_samples = defaultdict(list)
    with open(dep, newline="") as stream:
        for row in csv.DictReader(stream):
            trigger = row["trigger_pc"]
            pc = row["pc"]
            kind = row["kind"]
            by_trigger[trigger][pc] += 1
            kind_counts[kind] += 1
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
    print("CAMEL_DVR_TRACE_SUMMARY_WRITTEN", root)


if __name__ == "__main__":
    main()
