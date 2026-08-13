#!/usr/bin/env python3
"""Software BFS oracle emitting the complete DVR dependency pipeline as JSONL."""
import argparse, json

STAGES = ("stride_detection", "discovery_chain", "flr", "loop_bound",
          "lane_generation", "branch_mask", "helper_memory_request")

def emit(out, **event):
    out.write(json.dumps(event, sort_keys=True) + "\n")

def lane(out, mode, invocation, lane_id, trigger, bound, chain, active, requests):
    base = dict(mode=mode, invocation=invocation, lane=lane_id)
    emit(out, stage=STAGES[0], trigger=trigger, stride=1, detected=True, **base)
    emit(out, stage=STAGES[1], trigger=trigger, chain=chain, **base)
    emit(out, stage=STAGES[2], flr=chain[-1], valid=True, **base)
    emit(out, stage=STAGES[3], begin=0, end=bound, valid=0 <= lane_id < bound, **base)
    emit(out, stage=STAGES[4], value=trigger, active=active, **base)
    emit(out, stage=STAGES[5], active=active, mask_bit=1 if active else 0,
         reconverge="loop_latch", **base)
    for kind, address, value in requests:
        emit(out, stage=STAGES[6], kind=kind, address=address, value=value,
             active=active, **base)

def run(graph, source, out):
    adj = graph["adjacency"]
    n = len(adj); parent = [-1] * n; parent[source] = source
    frontier = [source]; invocation = 0
    while frontier:
        nxt = []
        for qidx, u in enumerate(frontier):
            neighbors = adj[u]
            for vidx, v in enumerate(neighbors):
                active = parent[v] < 0
                requests = [("queue_load", qidx, u),
                            ("index_begin", u, 0),
                            ("neighbor_load", vidx, v),
                            ("parent_load", v, parent[v])]
                if active:
                    parent[v] = u; nxt.append(v)
                    requests.append(("parent_cas", v, u))
                lane(out, "top_down", invocation, vidx, u, len(neighbors),
                     ["queue_load", "index_load", "neighbor_load", "parent_load"],
                     active, requests)
        frontier = nxt; invocation += 1
    emit(out, stage="summary", nodes=n, invocations=invocation,
         visited=sum(x >= 0 for x in parent), parent=parent)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("graph", help='JSON: {"adjacency": [[...], ...]}')
    p.add_argument("-s", "--source", type=int, default=0)
    p.add_argument("-o", "--output", default="-")
    a = p.parse_args()
    with open(a.graph) as f: graph = json.load(f)
    out = __import__("sys").stdout if a.output == "-" else open(a.output, "w")
    try: run(graph, a.source, out)
    finally:
        if out is not __import__("sys").stdout: out.close()
if __name__ == "__main__": main()
