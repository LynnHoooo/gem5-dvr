#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_ndm.riscv}"
SOURCE="${SOURCE:-$ROOT/benchmarks/dvr_ndm.c}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/dvr-ndm-e2e}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

require_nonzero() {
    local name="$1" value="$2"
    [[ -n "$value" && "$value" -gt 0 ]] || {
        printf 'error: expected %s > 0, got %s\n' "$name" "${value:-<missing>}" >&2
        exit 1
    }
}

if [[ ! -x "$BENCH" ]]; then
    test -f "$SOURCE"
    compiler="${CC:-}"
    if [[ -z "$compiler" ]]; then
        for candidate in riscv64-unknown-linux-gnu-gcc \
                        riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc; do
            if command -v "$candidate" >/dev/null 2>&1; then
                compiler="$candidate"
                break
            fi
        done
    fi
    [[ -n "$compiler" ]] || {
        printf 'error: no RISC-V compiler; set CC or provide BENCH=%s\n' \
            "$BENCH" >&2
        exit 1
    }
    mkdir -p "$OUT_ROOT"
    "$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops \
        -nostdlib -static -march=rv64gc -mabi=lp64d \
        -o "$OUT_ROOT/dvr_ndm.riscv" "$SOURCE"
    BENCH="$OUT_ROOT/dvr_ndm.riscv"
fi

test -x "$GEM5"
mkdir -p "$OUT_ROOT"
OUT="$OUT_ROOT/run"
mkdir -p "$OUT"
"$GEM5" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" \
    --cmd="$BENCH" --dvr --dvr-mode=nested \
    >"$OUT/stdout.log" 2>&1
grep -q 'exiting with last active thread context' "$OUT/stdout.log"
test -s "$OUT/stats.txt"

stats="$OUT/stats.txt"
attempts="$(read_stat "$stats" system.cpu.dvrNDMAttempts)"
branch_inversions="$(read_stat "$stats" system.cpu.dvrNDMBranchInversions)"
outer_found="$(read_stat "$stats" system.cpu.dvrNDMOuterFound)"
ndm_outer="$(read_stat "$stats" system.cpu.dvrNDMOuterInvocations)"
batches="$(read_stat "$stats" system.cpu.dvrNestedFlattenBatches)"
outer="$(read_stat "$stats" system.cpu.dvrNestedOuterInstances)"
inner="$(read_stat "$stats" system.cpu.dvrNestedInnerLanes)"
flattened="$(read_stat "$stats" system.cpu.dvrNestedFlattenedLanes)"
expected="$(read_stat "$stats" system.cpu.dvrNestedFlattenExpectedLanes)"
checks="$(read_stat "$stats" system.cpu.dvrNestedFlattenInvariantChecks)"
failures="$(read_stat "$stats" system.cpu.dvrNestedFlattenInvariantFailures)"
generated="$(read_stat "$stats" system.cpu.dvrNestedHelpersGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrNestedHelpersIssued)"
completed="$(read_stat "$stats" system.cpu.dvrNestedHelpersCompleted)"
targets="$(read_stat "$stats" system.cpu.dvrNestedReplayTargetsGenerated)"

require_nonzero ndm_attempts "$attempts"
require_nonzero branch_inversions "$branch_inversions"
require_nonzero ndm_outer_invocations "$ndm_outer"
require_nonzero ndm_outer_found "$outer_found"
require_nonzero nested_batches "$batches"
require_nonzero nested_outer_instances "$outer"
require_nonzero nested_flattened_lanes "$flattened"
require_nonzero dependent_replay_targets "$targets"
require_nonzero nested_helpers_generated "$generated"
require_nonzero nested_helpers_issued "$issued"
require_nonzero nested_helpers_completed "$completed"

[[ "$outer" -ge $((2 * batches)) ]] || {
    printf 'error: nested outer instances=%s < 2*batches=%s\n' \
        "$outer" "$((2 * batches))" >&2
    exit 1
}
[[ "$flattened" -eq "$expected" ]] || {
    printf 'error: flattened=%s expected=%s\n' "$flattened" "$expected" >&2
    exit 1
}
[[ "$checks" -eq "$batches" && "$failures" -eq 0 ]] || {
    printf 'error: flatten invariant checks=%s batches=%s failures=%s\n' \
        "$checks" "$batches" "$failures" >&2
    exit 1
}
[[ "$flattened" -le "$inner" ]] || {
    printf 'error: flattened=%s > inner=%s\n' "$flattened" "$inner" >&2
    exit 1
}

printf 'DVR_NDM_E2E_PASSED attempts=%s branch_inversions=%s outer_found=%s '\
'ndm_outer=%s batches=%s outer_instances=%s inner_lanes=%s '\
'flattened_lanes=%s expected_flattened=%s replay_targets=%s generated=%s '\
'issued=%s completed=%s\n' \
    "$attempts" "$branch_inversions" "$outer_found" "$ndm_outer" \
    "$batches" "$outer" "$inner" "$flattened" "$expected" "$targets" \
    "$generated" "$issued" "$completed"
