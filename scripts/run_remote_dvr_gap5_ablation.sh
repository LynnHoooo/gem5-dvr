#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
GAP_ROOT="${GAP_ROOT:-$HOME/dvr-repro/source/gapbs}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/gap5-ablation-s10}"
ALL_MODES=(baseline vr_like offload discovery full nested)
if [[ -n "${ONLY_MODE:-}" ]]; then
    MODES=("$ONLY_MODE")
else
    MODES=("${ALL_MODES[@]}")
fi
BENCHES=(bc bfs cc pr sssp)
mkdir -p "$OUT_ROOT"

run_case() {
    local bench="$1" mode="$2" out="$OUT_ROOT/$bench/$mode"
    local args=()
    if [[ "$mode" != baseline ]]; then
        cli_mode="$mode"
        [[ "$mode" == vr_like ]] && cli_mode=vr
        args=(--dvr --dvr-mode="$cli_mode" --dvr-quality-probe)
    fi
    rm -rf "$out"
    mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" \
        --cmd="$GAP_ROOT/$bench" --options='-g 10 -n 1' \
        "${args[@]}" >"$out/stdout.log" 2>&1
    grep -q 'exiting with last active thread context' "$out/stdout.log"
    test -s "$out/stats.txt"
}

for mode in "${MODES[@]}"; do
    pids=()
    for bench in "${BENCHES[@]}"; do
        run_case "$bench" "$mode" &
        pids+=("$!")
    done
    for pid in "${pids[@]}"; do
        wait "$pid"
    done
done

statv() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
csv="$OUT_ROOT/summary.csv"
printf 'workload,mode,cycles,ipc,demand_misses,helper_generated,helper_issued,resource_conflicts,nested_batches,outer_instances,flattened_lanes,fill_accuracy,coverage,timeliness,pollution_evictions\n' >"$csv"
for bench in "${BENCHES[@]}"; do
    for mode in "${ALL_MODES[@]}"; do
        stats="$OUT_ROOT/$bench/$mode/stats.txt"
        printf '%s,%s,%s\n' "$bench" "$mode" "$(IFS=,; echo "$(statv "$stats" simTicks),$(statv "$stats" system.cpu.ipc),$(statv "$stats" system.cpu.dcache.demandMisses::total),$(statv "$stats" system.cpu.dvrPrefetchesGenerated),$(statv "$stats" system.cpu.dvrPrefetchesIssued),$(statv "$stats" system.cpu.dvrResourceConflicts),$(statv "$stats" system.cpu.dvrNestedFlattenBatches),$(statv "$stats" system.cpu.dvrNestedOuterInstances),$(statv "$stats" system.cpu.dvrNestedFlattenedLanes),$(statv "$stats" system.cpu.dvr_quality_probe.fillAccuracy),$(statv "$stats" system.cpu.dvr_quality_probe.coverage),$(statv "$stats" system.cpu.dvr_quality_probe.timeliness),$(statv "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)")" >>"$csv"
    done
done
cat "$csv"
echo GAP5_DVR_ABLATION_PASSED
