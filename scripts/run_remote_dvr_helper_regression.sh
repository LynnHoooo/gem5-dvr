#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-helper-regression}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"

read_stat() { awk -v name="$2" '$1 == name {print $2; exit}' "$1"; }
positive() {
    local name="$1" value="$2"
    [[ -n "$value" && "$value" -gt 0 ]] || {
        printf 'error: expected %s > 0, got %s\n' "$name" "${value:-<missing>}" >&2
        exit 1
    }
}

[[ -x "$GEM5" ]] || { printf 'error: missing gem5: %s\n' "$GEM5" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { printf 'error: missing config: %s\n' "$CONFIG" >&2; exit 1; }

run_case() {
    local name="$1" mode="$2" bench="$3"
    local out="$OUT_ROOT/$RUN_ID/$name"
    [[ -x "$bench" ]] || { printf 'error: missing benchmark: %s\n' "$bench" >&2; exit 1; }
    mkdir -p "$out"
    "$GEM5" --outdir="$out" "$CONFIG" --cmd="$bench" --dvr \
        --dvr-mode="$mode" --dvr-vector-chunks >"$out/stdout.log" 2>&1
    [[ -s "$out/stats.txt" ]] || { printf 'error: no stats: %s\n' "$out" >&2; exit 1; }
    printf '%s\n' "$out"
}

branch_stats="$(run_case branch full "$BENCH_ROOT/dvr_divergent.riscv")"
branch_decoded="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrHelperDynUopsDecoded)"
branch_issued="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrHelperDynUopsIssued)"
branch_completed="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrHelperDynUopsCompleted)"
branch_width="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrVIRContinuationMaxGroupWidth)"
branch_divergent="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrDivergentBranches)"
branch_reconverged="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrReconvergences)"
branch_alt_complete="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrAlternatePathCompleteHits)"
branch_alt_uops="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrAlternatePathUopsReplayed)"
branch_alt_targets="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrAlternatePathDependentTargets)"
branch_alt_resumes="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrReconvergenceResumeSuccesses)"
branch_alt_covered="$(read_stat "$branch_stats/stats.txt" system.cpu.dvrAlternatePathDemandCovered)"
positive branch_dyn_uops "$branch_decoded"
[[ "$branch_decoded" -eq "$branch_issued" && "$branch_issued" -eq "$branch_completed" ]] || {
    printf 'error: branch helper lifecycle is not conserved\n' >&2; exit 1;
}
[[ "$branch_width" -ge 2 ]] || {
    printf 'error: branch helper did not form a multi-lane PC group: %s\n' "$branch_width" >&2
    exit 1
}
positive branch_divergence "$branch_divergent"
positive branch_reconvergence "$branch_reconverged"
positive alternate_path_complete_hits "$branch_alt_complete"
positive alternate_path_uops_replayed "$branch_alt_uops"
positive alternate_path_dependent_targets "$branch_alt_targets"
positive alternate_path_reconvergence_resumes "$branch_alt_resumes"
positive alternate_path_demand_covered "$branch_alt_covered"

nested_stats="$(run_case nested nested "$BENCH_ROOT/dvr_nested.riscv")"
for stat in dvrNDMAttempts dvrNDMBranchInversions dvrNDMOuterFound \
            dvrNDMOuterInvocations dvrNestedFlattenBatches \
            dvrNestedOuterInstances dvrNestedFlattenedLanes \
            dvrNestedFlattenExpectedLanes dvrNestedHelpersGenerated \
            dvrNestedHelpersIssued dvrNestedHelpersCompleted; do
    value="$(read_stat "$nested_stats/stats.txt" "system.cpu.$stat")"
    printf '%s=%s\n' "$stat" "$value"
    positive "$stat" "$value"
done
nested_batches="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedFlattenBatches)"
nested_outer="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedOuterInstances)"
nested_flat="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedFlattenedLanes)"
nested_expected="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedFlattenExpectedLanes)"
nested_failures="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedFlattenInvariantFailures)"
nested_generated="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedHelpersGenerated)"
[[ "$nested_outer" -ge $((2 * nested_batches)) ]] || exit 1
[[ "$nested_flat" -eq "$nested_expected" && "$nested_failures" -eq 0 ]] || exit 1
[[ "$nested_generated" -ge "$nested_flat" ]] || exit 1

variable_stats="$(run_case variable nested "$BENCH_ROOT/dvr_nested_variable.riscv")"
variable_batches="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedVariableLaneBatches)"
positive variable_lane_batches "$variable_batches"

printf 'DVR_HELPER_REGRESSION_PASSED run=%s branch_stats=%s nested_stats=%s variable_stats=%s branch_max_group_width=%s alternate_complete=%s alternate_uops=%s alternate_targets=%s alternate_resumes=%s alternate_covered=%s nested_flattened_lanes=%s variable_lane_batches=%s\n' \
    "$RUN_ID" "$branch_stats" "$nested_stats" "$variable_stats" \
    "$branch_width" "$branch_alt_complete" "$branch_alt_uops" \
    "$branch_alt_targets" "$branch_alt_resumes" "$branch_alt_covered" \
    "$nested_flat" "$variable_batches"
