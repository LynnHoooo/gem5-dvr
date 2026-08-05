#!/usr/bin/env bash
set -euo pipefail

# Reproduce the DVR ablation represented by Figure 8:
#   VR -> Offload -> +Discovery -> +Multiple/Nested
#
# Example on the server:
#   ROOT="$HOME/dvr-repro/source/gem5-runahead-dev-pre" \
#   BENCH_ROOT="$HOME/dvr-repro/source/gapbs" \
#   BENCHES_CSV=bc,bfs,cc,pr,sssp,hpc-db \
#   scripts/run_remote_dvr_figure8.sh
#
# The script intentionally keeps the workload command line configurable.  The
# default GAPBS options are suitable for the small reproduction runs; callers
# can override OPTIONS when using a different benchmark suite.

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$HOME/dvr-repro/source/gapbs}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-figure8}"
BENCHES_CSV="${BENCHES_CSV:-bc,bfs,cc,pr,sssp,hpc-db}"
OPTIONS="${OPTIONS:--g 10 -n 1}"

gem5="${GEM5_BIN:-$HOME/build/RISCV/gem5.opt}"
if [[ ! -x "$gem5" ]]; then
    gem5="$ROOT/build/RISCV/gem5.opt"
fi
config="$ROOT/configs/dvr/table1_se.py"
test -x "$gem5"
test -f "$config"

IFS=',' read -r -a benches <<< "$BENCHES_CSV"
(( ${#benches[@]} > 0 ))

# The names match the four bars in Figure 8.  "full" means ordinary DVR with
# offload plus Discovery; "nested" adds multi-invocation aggregation.
modes=(VR Offload Discovery Multiple)
all_modes=(Baseline "${modes[@]}")

mkdir -p "$OUT_ROOT"

run_case() {
    local bench="$1" mode="$2"
    local binary="$BENCH_ROOT/$bench"
    local out="$OUT_ROOT/$bench/$mode"
    local args=()

    test -x "$binary"
    rm -rf "$out"
    mkdir -p "$out"

    case "$mode" in
        Baseline) args=() ;;
        # Figure 8 compares the DVR mechanisms on the same vector-runahead
        # substrate.  Leaving vector chunks disabled silently turns every
        # DVR bar into the scalar helper model and prevents source responses
        # from entering the vector replay path.
        VR)       args=(--dvr --dvr-mode=vr --dvr-vector-chunks) ;;
        Offload)  args=(--dvr --dvr-mode=offload --dvr-vector-chunks) ;;
        Discovery) args=(--dvr --dvr-mode=full --dvr-vector-chunks) ;;
        Multiple) args=(--dvr --dvr-mode=nested --dvr-vector-chunks) ;;
        *)        echo "unknown mode: $mode" >&2; exit 2 ;;
    esac

    "$gem5" --outdir="$out" "$config" --cmd="$binary" \
        --options="$OPTIONS" --dvr-quality-probe "${args[@]}" \
        >"$out/stdout.log" 2>&1
    grep -q 'exiting with last active thread context' "$out/stdout.log"
    test -s "$out/stats.txt"
}

read_stat() {
    awk -v name="$2" '$1 == name {print $2; exit}' "$1"
}

for bench in "${benches[@]}"; do
    binary="$BENCH_ROOT/$bench"
    test -x "$binary"
    run_case "$bench" Baseline
    run_case "$bench" VR
    run_case "$bench" Offload
    run_case "$bench" Discovery
    run_case "$bench" Multiple
done

raw="$OUT_ROOT/raw.csv"
summary="$OUT_ROOT/figure8.csv"
printf 'workload,mode,cycles,ipc,demand_misses,helper_generated,helper_issued,helper_completed,normalized_ipc,fill_accuracy,coverage,timeliness,pollution_evictions\n' > "$raw"

for bench in "${benches[@]}"; do
    for mode in "${all_modes[@]}"; do
        stats="$OUT_ROOT/$bench/$mode/stats.txt"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$bench" "$mode" \
            "$(read_stat "$stats" system.cpu.numCycles)" \
            "$(read_stat "$stats" system.cpu.ipc)" \
            "$(read_stat "$stats" system.cpu.dcache.ReadReq.misses::cpu.data)" \
            "$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)" \
            "$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)" \
            "$(read_stat "$stats" system.cpu.dvrPrefetchesCompleted)" \
            "" \
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.fillAccuracy)" \
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.coverage)" \
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.timeliness)" \
            "$(read_stat "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)" \
            >> "$raw"
    done
done

# Normalize with a second, simple pass so the raw CSV remains useful for
# debugging and exact reproduction.
awk -F, '
NR == 1 { print; next }
{
    if ($2 == "Baseline") base[$1] = $4
    row[NR] = $0
    n = NR
}
END {
    for (i = 2; i <= n; ++i) {
        split(row[i], f, ",")
        norm = f[4] / base[f[1]]
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%.6f,%s,%s,%s,%s\n", \
            f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], norm, \
            f[10], f[11], f[12], f[13]
        sum[f[2]] += 1.0 / norm
        count[f[2]]++
    }
    for (mode in count)
        printf "H-mean,%s,,,,,,,% .6f,,,,\n", mode, count[mode] / sum[mode]
}' "$raw" > "$summary.tmp"
mv "$summary.tmp" "$summary"

cat "$summary"
printf 'DVR_FIGURE8_PASSED raw=%s summary=%s\n' "$raw" "$summary"
