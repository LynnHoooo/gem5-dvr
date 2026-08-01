#!/usr/bin/env bash
set -euo pipefail

RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"
GAP_ROOT="${GAP_ROOT:-$HOME/dvr-repro/source/gapbs}"
OUT="$RESULT_ROOT/gap5-s10"

read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
run_dir() {
    local bench="$1" mode="$2"
    if [[ "$bench" == bfs ]]; then
        printf '%s/gap-bfs-s10/%s' "$RESULT_ROOT" "$mode"
    else
        printf '%s/gap5-s10/%s/%s' "$RESULT_ROOT" "$bench" "$mode"
    fi
}

mkdir -p "$OUT"
{
    printf 'gap_sha=%s\n' \
        "$(git -C "$GAP_ROOT" rev-parse HEAD)"
    printf 'input=%s\n' '-g 10 -n 1'
    for bench in bc bfs cc pr sssp; do
        printf '%s_sha256=%s\n' "$bench" \
            "$(sha256sum "$GAP_ROOT/$bench" | awk '{print $1}')"
    done
} >"$OUT/manifest.txt"

printf 'workload,mode,ticks,ipc,demand_misses,helper_issued,conflicts,nested_batches,outer_instances,flattened_lanes,fill_accuracy,coverage,timeliness,pollution_evictions\n' \
    >"$OUT/summary.csv"

for bench in bc bfs cc pr sssp; do
    for mode in baseline full nested; do
        dir="$(run_dir "$bench" "$mode")"
        stats="$dir/stats.txt"
        test -s "$stats"
        grep -q 'exiting with last active thread context' "$dir/stdout.log"
        values=(
            "$(read_stat "$stats" simTicks)"
            "$(read_stat "$stats" system.cpu.ipc)"
            "$(read_stat "$stats" system.cpu.dcache.demandMisses::total)"
            "$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
            "$(read_stat "$stats" system.cpu.dvrResourceConflicts)"
            "$(read_stat "$stats" system.cpu.dvrNestedFlattenBatches)"
            "$(read_stat "$stats" system.cpu.dvrNestedOuterInstances)"
            "$(read_stat "$stats" system.cpu.dvrNestedFlattenedLanes)"
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.fillAccuracy)"
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.coverage)"
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.timeliness)"
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)"
        )
        printf '%s,%s,%s\n' "$bench" "$mode" \
            "$(IFS=,; echo "${values[*]}")" >>"$OUT/summary.csv"
    done
done

cat "$OUT/summary.csv"
echo DVR_GAP5_S10_RESULTS_VALIDATED
