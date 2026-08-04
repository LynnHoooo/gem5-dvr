#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-anchor}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
BENCHES="${BENCHES:-dvr_dependent.riscv}"
OPTIONS="${OPTIONS:-}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"

statv() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

mkdir -p "$OUT_ROOT"
IFS=',' read -r -a bench_list <<< "$BENCHES"

for bench in "${bench_list[@]}"; do
    test -x "$BENCH_ROOT/$bench"
    for mode in baseline offload full; do
        out="$OUT_ROOT/${bench%.riscv}/$mode"
        mkdir -p "$out"
        args=()
        if [[ "$mode" != baseline ]]; then
            args=(--dvr --dvr-mode="$mode")
        fi
        "$GEM5" --outdir="$out" "$ROOT/configs/dvr/table1_se.py"            --cmd="$BENCH_ROOT/$bench" --options="$OPTIONS"            --dvr-quality-probe "${args[@]}" >"$out/stdout.log" 2>&1
        grep -q 'exiting with last active thread context' "$out/stdout.log"
        test -s "$out/stats.txt"
    done
done

csv="$OUT_ROOT/summary.csv"
printf '%s\n'    'workload,mode,cycles,ipc,demand_misses,trigger_candidates,discovery_starts,discovery_completions,loop_bound_matches,average_lanes,helper_generated,dependent_generated,helper_issued,helper_completed,possibly_useful,late,quality_issued,quality_completed,coverage,accuracy,timeliness,outstanding_line_sum,outstanding_line_samples,outstanding_line_peak'    >"$csv"

for bench in "${bench_list[@]}"; do
    workload="${bench%.riscv}"
    for mode in baseline offload full; do
        stats="$OUT_ROOT/$workload/$mode/stats.txt"
        samples="$(statv "$stats" system.cpu.dvrOutstandingPrefetchLineSamples)"
        sum="$(statv "$stats" system.cpu.dvrOutstandingPrefetchLineSum)"
        lanes=0
        if [[ "$(statv "$stats" system.cpu.dvrLaneCountSamples)" -gt 0 ]]; then
            lanes="$(awk -v total="$(statv "$stats" system.cpu.dvrTotalActiveLanes)" -v count="$(statv "$stats" system.cpu.dvrLaneCountSamples)" 'BEGIN { printf "%.6f", total/count }')"
        fi
        printf '%s\n' "$workload,$mode,$(statv "$stats" simTicks),$(statv "$stats" system.cpu.ipc),$(statv "$stats" system.cpu.dcache.demandMisses::total),$(statv "$stats" system.cpu.dvrStrideCandidates),$(statv "$stats" system.cpu.dvrDiscoveryStarts),$(statv "$stats" system.cpu.dvrDiscoveryCompletions),$(statv "$stats" system.cpu.dvrLoopBoundMatches),$lanes,$(statv "$stats" system.cpu.dvrPrefetchesGenerated),$(statv "$stats" system.cpu.dvrDependentPrefetchesGenerated),$(statv "$stats" system.cpu.dvrPrefetchesIssued),$(statv "$stats" system.cpu.dvrPrefetchesCompleted),$(statv "$stats" system.cpu.dvrPrefetchesPossiblyUseful),$(statv "$stats" system.cpu.dvrPrefetchesLate),$(statv "$stats" system.cpu.dvrQualityIssuedBytes),$(statv "$stats" system.cpu.dvrQualityCompletedBytes),$(statv "$stats" system.cpu.dvr_quality_probe.coverage),$(statv "$stats" system.cpu.dvr_quality_probe.fillAccuracy),$(statv "$stats" system.cpu.dvr_quality_probe.timeliness),$sum,$samples,$(statv "$stats" system.cpu.dvrOutstandingPrefetchLinePeak)" >>"$csv"
    done
done

cat "$csv"
printf 'DVR_ANCHOR_PASSED summary=%s\n' "$csv"
