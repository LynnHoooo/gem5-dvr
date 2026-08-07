#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
if [[ -n "${BENCH_ROOT:-}" ]]; then
    BENCH_ROOT="$BENCH_ROOT"
elif [[ -x "$REPO_ROOT/benchmarks/dvr_nested.riscv" ]]; then
    BENCH_ROOT="$REPO_ROOT/benchmarks"
else
    BENCH_ROOT="$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks"
fi
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-helper-regression}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"

read_stat() { awk -v name="$2" '$1 == name {print $2; exit}' "$1"; }
zero() {
    local name="$1" value="$2"
    [[ -n "$value" && "$value" -eq 0 ]] || {
        printf 'error: expected %s == 0, got %s\n' "$name" "${value:-<missing>}" >&2
        exit 1
    }
}
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
# Alternate-path cache training is intentionally covered by the dedicated
# run_remote_dvr_alternate_path.sh gate.  It is not a deterministic property
# of this ordinary divergence/M2 lifecycle run.

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
nested_source_completed="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrSourcePrefetchesCompleted)"
nested_replay="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedReplayAttempts)"
nested_targets="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrNestedReplayTargetsGenerated)"
nested_dependent_issued="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrDependentPrefetchesIssued)"
nested_dependent_completed="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrDependentPrefetchesCompleted)"
nested_source_issued="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrSourcePrefetchesIssued)"
nested_faults="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrPrefetchTranslationFaults)"
nested_mask_failures="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrVIRActiveMaskFailures)"
nested_stack_overflows="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrReconvergenceStackOverflows)"
nested_pending="$(read_stat "$nested_stats/stats.txt" system.cpu.dvrHelperLoadEntryPending)"
[[ "$nested_outer" -ge $((2 * nested_batches)) ]] || exit 1
[[ "$nested_flat" -eq "$nested_expected" && "$nested_failures" -eq 0 ]] || exit 1
[[ "$nested_generated" -ge "$nested_flat" ]] || exit 1
positive nested_source_completed "$nested_source_completed"
[[ "$nested_source_issued" -eq "$nested_source_completed" ]] || {
    printf 'error: nested source request lifecycle is not conserved\n' >&2; exit 1;
}
positive nested_replay_attempts "$nested_replay"
positive nested_replay_targets "$nested_targets"
positive nested_dependent_issued "$nested_dependent_issued"
[[ "$nested_dependent_issued" -eq "$nested_dependent_completed" ]] || {
    printf 'error: nested dependent request lifecycle is not conserved\n' >&2; exit 1;
}
zero nested_translation_faults "$nested_faults"
zero nested_active_mask_failures "$nested_mask_failures"
zero nested_reconvergence_stack_overflows "$nested_stack_overflows"
zero nested_helper_load_pending "$nested_pending"

variable_stats="$(run_case variable nested "$BENCH_ROOT/dvr_nested_variable.riscv")"
variable_batches="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedVariableLaneBatches)"
positive variable_lane_batches "$variable_batches"
variable_flat="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedFlattenedLanes)"
variable_expected="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedFlattenExpectedLanes)"
variable_failures="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedFlattenInvariantFailures)"
variable_replay="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedReplayAttempts)"
variable_targets="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrNestedReplayTargetsGenerated)"
variable_issued="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrDependentPrefetchesIssued)"
variable_completed="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrDependentPrefetchesCompleted)"
variable_faults="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrPrefetchTranslationFaults)"
variable_pending="$(read_stat "$variable_stats/stats.txt" system.cpu.dvrHelperLoadEntryPending)"
[[ "$variable_flat" -eq "$variable_expected" && "$variable_failures" -eq 0 ]] || exit 1
positive variable_replay_attempts "$variable_replay"
positive variable_replay_targets "$variable_targets"
positive variable_dependent_issued "$variable_issued"
[[ "$variable_issued" -eq "$variable_completed" ]] || exit 1
zero variable_translation_faults "$variable_faults"
zero variable_helper_load_pending "$variable_pending"

printf 'DVR_HELPER_REGRESSION_PASSED run=%s branch_stats=%s nested_stats=%s variable_stats=%s branch_max_group_width=%s nested_flattened_lanes=%s nested_replay_targets=%s nested_dependent_issued_completed=%s/%s variable_lane_batches=%s\n' \
    "$RUN_ID" "$branch_stats" "$nested_stats" "$variable_stats" \
    "$branch_width" "$nested_flat" "$nested_targets" \
    "$nested_dependent_issued" "$nested_dependent_completed" "$variable_batches"
