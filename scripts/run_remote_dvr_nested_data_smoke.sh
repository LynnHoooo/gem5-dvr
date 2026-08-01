#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-nested-data}"
mkdir -p "$OUT_ROOT"

read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
run_one() {
    local name="$1" bench="$2" out="$OUT_ROOT/$1"
    rm -rf "$out"; mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$ROOT/benchmarks/$bench" \
        --dvr --dvr-mode=nested
    local stats="$out/stats.txt"
    local batches outer inner flat variable generated issued
    batches="$(read_stat "$stats" system.cpu.dvrNestedFlattenBatches)"
    outer="$(read_stat "$stats" system.cpu.dvrNestedOuterInstances)"
    inner="$(read_stat "$stats" system.cpu.dvrNestedInnerLanes)"
    flat="$(read_stat "$stats" system.cpu.dvrNestedFlattenedLanes)"
    variable="$(read_stat "$stats" system.cpu.dvrNestedVariableLaneBatches)"
    generated="$(read_stat "$stats" system.cpu.dvrNestedHelpersGenerated)"
    issued="$(read_stat "$stats" system.cpu.dvrNestedHelpersIssued)"
    printf '%s batches=%s outer_instances=%s inner_lanes=%s flattened_lanes=%s variable_lane_batches=%s generated=%s issued=%s\n' \
        "$name" "$batches" "$outer" "$inner" "$flat" "$variable" \
        "$generated" "$issued"
    test "$batches" -gt 0
    test "$outer" -ge "$batches"
    test "$inner" -ge "$batches"
    test "$flat" -gt 0
    test "$flat" -le "$inner"
    test "$flat" -le $((128 * batches))
    test "$generated" -ge "$flat"
}

run_one ndm dvr_ndm.riscv
run_one nested dvr_nested.riscv
echo DVR_NESTED_DATA_PASSED
