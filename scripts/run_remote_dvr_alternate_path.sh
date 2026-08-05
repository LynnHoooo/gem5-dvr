#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks}"
BENCH="${BENCH:-$BENCH_ROOT/dvr_divergent.riscv}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-alternate-path}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"
OUT="$OUT_ROOT/$RUN_ID"
BASELINE="$OUT/baseline"
FULL="$OUT/full"
TRACE="$FULL/trace"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

positive() {
    local label="$1"
    local value="$2"
    [[ -n "$value" && "$value" -gt 0 ]] || {
        printf 'error: expected %s > 0, got %s\n' "$label" "${value:-<missing>}" >&2
        exit 1
    }
}

equal() {
    local label="$1"
    local lhs="$2"
    local rhs="$3"
    [[ "$lhs" == "$rhs" ]] || {
        printf 'error: expected %s=%s, got %s\n' "$label" "$rhs" "$lhs" >&2
        exit 1
    }
}

[[ -x "$GEM5" ]] || { printf 'error: missing gem5: %s\n' "$GEM5" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { printf 'error: missing config: %s\n' "$CONFIG" >&2; exit 1; }
[[ -x "$BENCH" ]] || { printf 'error: missing benchmark: %s\n' "$BENCH" >&2; exit 1; }
mkdir -p "$BASELINE" "$TRACE"

"$GEM5" --outdir="$BASELINE" "$CONFIG" --cmd="$BENCH" \
    >"$BASELINE/stdout.log" 2>&1
DVR_TRACE_DIR="$TRACE" "$GEM5" --outdir="$FULL" "$CONFIG" --cmd="$BENCH" \
    --dvr --dvr-mode=full --dvr-vector-chunks \
    >"$FULL/stdout.log" 2>&1

[[ -s "$BASELINE/stats.txt" && -s "$FULL/stats.txt" ]] || {
    printf 'error: baseline/full stats are missing\n' >&2
    exit 1
}
[[ -s "$TRACE/vectorization.csv" && -s "$TRACE/dependency_chain.csv" ]] || {
    printf 'error: DVR trace files are missing\n' >&2
    exit 1
}

baseline_committed="$(read_stat "$BASELINE/stats.txt" system.cpu.committedInsts)"
full_committed="$(read_stat "$FULL/stats.txt" system.cpu.committedInsts)"
equal committed_instructions "$full_committed" "$baseline_committed"

stats="$FULL/stats.txt"
complete_hits="$(read_stat "$stats" system.cpu.dvrAlternatePathCompleteHits)"
alternate_uops="$(read_stat "$stats" system.cpu.dvrAlternatePathUopsReplayed)"
alternate_targets="$(read_stat "$stats" system.cpu.dvrAlternatePathDependentTargets)"
alternate_covered="$(read_stat "$stats" system.cpu.dvrAlternatePathDemandCovered)"
alternate_resumes="$(read_stat "$stats" system.cpu.dvrReconvergenceResumeSuccesses)"
stack_overflows="$(read_stat "$stats" system.cpu.dvrReconvergenceStackOverflows)"
helper_decoded="$(read_stat "$stats" system.cpu.dvrHelperDynUopsDecoded)"
helper_issued="$(read_stat "$stats" system.cpu.dvrHelperDynUopsIssued)"
helper_completed="$(read_stat "$stats" system.cpu.dvrHelperDynUopsCompleted)"
max_group="$(read_stat "$stats" system.cpu.dvrVIRContinuationMaxGroupWidth)"

positive alternate_path_complete_hits "$complete_hits"
positive alternate_path_uops_replayed "$alternate_uops"
positive alternate_path_dependent_targets "$alternate_targets"
positive alternate_path_demand_covered "$alternate_covered"
positive alternate_path_reconvergence_resumes "$alternate_resumes"
positive same_pc_group_width "$max_group"
equal reconvergence_stack_overflows "$stack_overflows" 0
equal helper_dyn_uops_issued "$helper_issued" "$helper_decoded"
equal helper_dyn_uops_completed "$helper_completed" "$helper_issued"
[[ "$alternate_covered" -le "$alternate_targets" ]] || {
    printf 'error: demand coverage exceeds alternate target generation\n' >&2
    exit 1
}

read -r trace_alt_uops trace_alt_single trace_alt_partial trace_alt_lane_sum \
    < <(awk -F, '
        $2 == "alternate_path_uop" {
            count++;
            lanes = $5 + 0;
            sum += lanes;
            if (lanes == 1) single++;
            if (lanes > 1 && lanes < 8) partial++;
        }
        END { print count + 0, single + 0, partial + 0, sum + 0 }
    ' "$TRACE/vectorization.csv")
read -r trace_alt_targets < <(
    awk -F, '$2 == "alternate_replay_target" { count++ } END { print count + 0 }' \
        "$TRACE/dependency_chain.csv"
)

positive trace_alternate_path_uops "$trace_alt_uops"
positive trace_single_lane_alternate_uops "$trace_alt_single"
positive trace_partial_chunk_alternate_uops "$trace_alt_partial"
[[ "$trace_alt_lane_sum" -le "$alternate_uops" ]] || {
    printf 'error: persistent trace exceeds alternate uop total\n' >&2
    exit 1
}
untraced_alt_uops=$((alternate_uops - trace_alt_lane_sum))
equal trace_alternate_targets "$trace_alt_targets" "$alternate_targets"

printf 'DVR_ALTERNATE_PATH_PASSED run=%s output=%s committed=%s complete_hits=%s alternate_uops=%s alternate_targets=%s demand_covered=%s resumes=%s persistent_trace_uops=%s untraced_alt_uops=%s trace_single_lane=%s trace_partial_chunk=%s max_group=%s\n' \
    "$RUN_ID" "$OUT" "$full_committed" "$complete_hits" \
    "$alternate_uops" "$alternate_targets" "$alternate_covered" \
    "$alternate_resumes" "$trace_alt_lane_sum" "$untraced_alt_uops" \
    "$trace_alt_single" "$trace_alt_partial" "$max_group"
