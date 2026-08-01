#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-ablation}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
mkdir -p "$OUT_ROOT"

run_case() {
    local name="$1"; shift
    local out="$OUT_ROOT/$name"
    rm -rf "$out"; mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" \
        --dvr-quality-probe "$@"
}

statv() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }

run_case baseline
run_case vr_like --dvr --dvr-mode=vr
run_case offload --dvr --dvr-mode=offload
run_case discovery --dvr --dvr-mode=discovery
run_case full --dvr --dvr-mode=full
run_case nested --dvr --dvr-mode=nested

csv="$OUT_ROOT/summary.csv"
printf 'mode,cycles,ipc,demand_misses,helper_generated,helper_issued,resource_conflicts,fill_accuracy,coverage,timeliness,pollution_evictions\n' > "$csv"
for mode in baseline vr_like offload discovery full nested; do
    stats="$OUT_ROOT/$mode/stats.txt"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$mode" \
        "$(statv "$stats" system.cpu.numCycles)" \
        "$(statv "$stats" system.cpu.ipc)" \
        "$(statv "$stats" system.cpu.dcache.ReadReq.misses::cpu.data)" \
        "$(statv "$stats" system.cpu.dvrPrefetchesGenerated)" \
        "$(statv "$stats" system.cpu.dvrPrefetchesIssued)" \
        "$(statv "$stats" system.cpu.dvrResourceConflicts)" \
        "$(statv "$stats" system.cpu.dvr_quality_probe.fillAccuracy)" \
        "$(statv "$stats" system.cpu.dvr_quality_probe.coverage)" \
        "$(statv "$stats" system.cpu.dvr_quality_probe.timeliness)" \
        "$(statv "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)" \
        >> "$csv"
done
cat "$csv"
echo "DVR_ABLATION_PASSED summary=$csv"
