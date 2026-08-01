#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-nested-data}"
mkdir -p "$OUT_ROOT"

read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
compile_bench() {
    local source="$1" output="$2" compiler=""
    test -f "$source"
    for candidate in riscv64-unknown-linux-gnu-gcc \
                     riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler="$candidate"; break
        fi
    done
    test -n "$compiler"
    "$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops \
        -nostdlib -static -march=rv64gc -mabi=lp64d \
        -o "$output" "$source"
}

run_one() {
    local name="$1" bench="$2" require_batches="$3" require_variable="$4"
    local out="$OUT_ROOT/$1"
    if [[ ! -x "$ROOT/benchmarks/$bench" ]]; then
        compile_bench "$ROOT/benchmarks/${bench%.riscv}.c" \
            "$ROOT/benchmarks/$bench"
    fi
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
    if [[ "$require_batches" == 1 ]]; then
        test "$batches" -gt 0
        test "$outer" -ge $((2 * batches))
        test "$inner" -ge "$batches"
        test "$flat" -gt 0
        test "$flat" -le "$inner"
        test "$flat" -le $((128 * batches))
        test "$generated" -ge "$flat"
    fi
    if [[ "$require_variable" == 1 ]]; then
        test "$variable" -gt 0
    fi
}

run_one ndm dvr_ndm.riscv 0 0
run_one nested dvr_nested.riscv 1 0
run_one variable dvr_nested_variable.riscv 1 1
echo DVR_NESTED_DATA_PASSED
