#!/usr/bin/env python3
"""Normalize gem5 DVR JSONL and identify the first broken BFS oracle stage."""
import argparse, collections, json, sys

ORDER = ["stride_detection", "discovery_chain", "flr", "loop_bound",
         "lane_generation", "branch_mask", "helper_memory_request"]
KINDS = {
 "stride_detection": {"discovery_start", "outer_stride"},
 "discovery_chain": {"tainted", "nested_tainted", "discovery_complete"},
 "flr": {"flr", "nested_flr"},
 "loop_bound": {"discovery_launch", "loop_bound_fallback_launch", "invocation"},
 "lane_generation": {"source_lane", "vr_lane", "nested_source_lane", "flatten_batch"},
 "branch_mask": {"vir_issue_group", "reconvergence_branch", "reconvergence_push",
                  "reconvergence_pop", "alternate_path_uop"},
 "helper_memory_request": {"replay_target", "alternate_replay_target",
                           "dependent_prefetch"},
}
def events(path):
    with open(path) as f:
        for no, line in enumerate(f, 1):
            try:
                e=json.loads(line)
                if isinstance(e, dict): yield no,e
            except (ValueError, TypeError): pass
def main():
    p=argparse.ArgumentParser(); p.add_argument("oracle"); p.add_argument("dvr")
    p.add_argument("--min-active-lanes", type=int, default=80); a=p.parse_args()
    expected=collections.Counter(e["stage"] for _,e in events(a.oracle) if e.get("stage") in ORDER)
    actual=collections.Counter(); samples={}; widths=[]
    for no,e in events(a.dvr):
        kind=e.get("kind","")
        for stage,kinds in KINDS.items():
            if kind in kinds:
                actual[stage]+=1; samples.setdefault(stage,(no,e))
        if kind in ("vir_issue_group","flatten_batch") and isinstance(e.get("lanes"),int):
            widths.append(e["lanes"])
    first=None
    for stage in ORDER:
        status="OK" if actual[stage] else "MISSING"
        print("%-24s oracle=%-8d dvr=%-8d %s"%(stage,expected[stage],actual[stage],status))
        if first is None and not actual[stage]: first=stage
    if widths:
        avg=sum(widths)/len(widths); print("active_lanes avg=%.2f max=%d samples=%d"%(avg,max(widths),len(widths)))
        if avg < a.min_active_lanes and first is None: first="lane_generation/branch_mask (low active lanes)"
    else:
        print("active_lanes: no VIR issue groups"); first=first or "lane_generation"
    print("FIRST_DIVERGENCE:", first or "none in stage presence; run value-aligned comparison")
    return 1 if first else 0
if __name__ == "__main__": sys.exit(main())
