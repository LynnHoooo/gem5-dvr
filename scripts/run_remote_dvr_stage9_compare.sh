#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"

run_case() {
    local name="$1"
    shift
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" "$@"
}

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

run_case dvr-stage9-baseline
run_case dvr-stage9-dvr --dvr

base="$RESULT_ROOT/dvr-stage9-baseline/stats.txt"
dvr="$RESULT_ROOT/dvr-stage9-dvr/stats.txt"
base_cycles="$(read_stat "$base" system.cpu.numCycles)"
dvr_cycles="$(read_stat "$dvr" system.cpu.numCycles)"
base_misses="$(read_stat "$base" system.cpu.dcache.ReadReq.misses::cpu.data)"
dvr_misses="$(read_stat "$dvr" system.cpu.dcache.ReadReq.misses::cpu.data)"
test "$dvr_misses" -lt "$base_misses"

miss_reduction="$(awk -v b="$base_misses" -v d="$dvr_misses" \
    'BEGIN { printf "%.2f", 100.0 * (b - d) / b }')"
speedup="$(awk -v b="$base_cycles" -v d="$dvr_cycles" \
    'BEGIN { printf "%.6f", b / d }')"

printf 'DVR_STAGE9_COMPARE_PASSED baseline_cycles=%s dvr_cycles=%s speedup=%s baseline_misses=%s dvr_misses=%s miss_reduction_pct=%s\n' \
    "$base_cycles" "$dvr_cycles" "$speedup" "$base_misses" "$dvr_misses" \
    "$miss_reduction"
