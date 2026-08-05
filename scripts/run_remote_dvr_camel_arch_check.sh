#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
BENCH="${BENCH:-/home/lynnhoo/dvr-repro/results/camel-dvr-trace-c_lw-full/camel.riscv}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-camel-arch-check}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"

read_stat() {
    awk -v name="$2" '$1 == name {print $2; exit}' "$1"
}

positive() {
    local name="$1" value="$2"
    [[ -n "$value" && "$value" -gt 0 ]] || {
        printf 'error: expected %s > 0, got %s\n' "$name" "${value:-<missing>}" >&2
        exit 1
    }
}

equal() {
    local name="$1" lhs="$2" rhs="$3"
    [[ "$lhs" == "$rhs" ]] || {
        printf 'error: expected %s: %s != %s\n' "$name" "$lhs" "$rhs" >&2
        exit 1
    }
}

[[ -x "$GEM5" ]] || { printf 'error: missing gem5: %s\n' "$GEM5" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { printf 'error: missing config: %s\n' "$CONFIG" >&2; exit 1; }
[[ -x "$BENCH" ]] || { printf 'error: missing Camel benchmark: %s\n' "$BENCH" >&2; exit 1; }

run_case() {
    local name="$1"
    shift
    local out="$OUT_ROOT/$RUN_ID/$name"
    mkdir -p "$out/trace"
    DVR_TRACE_DIR="$out/trace" "$GEM5" --outdir="$out" "$CONFIG" \
        --cmd="$BENCH" "$@" >"$out/stdout.log" 2>&1
    [[ -s "$out/stats.txt" ]] || {
        printf 'error: no stats for %s\n' "$name" >&2
        exit 1
    }
    printf '%s\n' "$out"
}

baseline="$(run_case baseline)"
baseline_stats="$baseline/stats.txt"
baseline_result="$(awk '/^Result / {print $2; exit}' "$baseline/stdout.log")"
baseline_committed="$(read_stat "$baseline/stats.txt" system.cpu.committedInsts)"
positive baseline_committed "$baseline_committed"

declare -A CASE_DIRS
CASE_DIRS[baseline]="$baseline"

run_and_check_result() {
    local name="$1"
    shift
    local out
    out="$(run_case "$name" "$@")"
    local result committed
    result="$(awk '/^Result / {print $2; exit}' "$out/stdout.log")"
    committed="$(read_stat "$out/stats.txt" system.cpu.committedInsts)"
    equal "$name result" "$baseline_result" "$result"
    equal "$name committedInsts" "$baseline_committed" "$committed"
    CASE_DIRS["$name"]="$out"
}

run_and_check_result vr --dvr --dvr-mode=vr --dvr-vector-chunks
vr_stats="${CASE_DIRS[vr]}/stats.txt"
positive vr_stride_candidates "$(read_stat "$vr_stats" system.cpu.dvrStrideCandidates)"
equal vr_discovery_starts 0 "$(read_stat "$vr_stats" system.cpu.dvrDiscoveryStarts)"
equal vr_vector_programs 0 "$(read_stat "$vr_stats" system.cpu.dvrVectorProgramsBuilt)"
positive vr_active_lanes "$(read_stat "$vr_stats" system.cpu.dvrVectorActiveLanes)"

run_and_check_result offload --dvr --dvr-mode=offload --dvr-vector-chunks
offload_stats="${CASE_DIRS[offload]}/stats.txt"
positive offload_stride_candidates "$(read_stat "$offload_stats" system.cpu.dvrStrideCandidates)"
positive offload_vector_programs "$(read_stat "$offload_stats" system.cpu.dvrVectorProgramsBuilt)"
equal offload_dyn_uop_issue \
    "$(read_stat "$offload_stats" system.cpu.dvrHelperDynUopsDecoded)" \
    "$(read_stat "$offload_stats" system.cpu.dvrHelperDynUopsIssued)"
equal offload_dyn_uop_completion \
    "$(read_stat "$offload_stats" system.cpu.dvrHelperDynUopsIssued)" \
    "$(read_stat "$offload_stats" system.cpu.dvrHelperDynUopsCompleted)"

run_and_check_result discovery --dvr --dvr-mode=discovery --dvr-vector-chunks
discovery_stats="${CASE_DIRS[discovery]}/stats.txt"
positive discovery_starts "$(read_stat "$discovery_stats" system.cpu.dvrDiscoveryStarts)"
positive discovery_completions "$(read_stat "$discovery_stats" system.cpu.dvrDiscoveryCompletions)"
positive discovery_loop_bound_matches "$(read_stat "$discovery_stats" system.cpu.dvrLoopBoundMatches)"
equal discovery_vector_programs 0 "$(read_stat "$discovery_stats" system.cpu.dvrVectorProgramsBuilt)"
equal discovery_prefetches_issued 0 "$(read_stat "$discovery_stats" system.cpu.dvrPrefetchesIssued)"

run_and_check_result full-vector --dvr --dvr-mode=full --dvr-vector-chunks
full_stats="${CASE_DIRS[full-vector]}/stats.txt"
for stat in dvrStrideCandidates dvrDiscoveryStarts dvrDiscoveryCompletions \
            dvrLoopBoundMatches dvrHelperVRATPrograms dvrHelperVRATWrites \
            dvrVectorizerSourceLanes dvrVectorizerDependentLanes \
            dvrVIRActiveMaskChecks dvrVectorChunkRequests \
            dvrVectorActiveLanes dvrDependentPrefetchesGenerated \
            dvrPrefetchesPossiblyUseful dvrDependentDemandCovered; do
    positive "full_$stat" "$(read_stat "$full_stats" "system.cpu.$stat")"
done
for pair in \
    "dvrHelperDynUopsDecoded dvrHelperDynUopsIssued" \
    "dvrHelperDynUopsIssued dvrHelperDynUopsCompleted" \
    "dvrPrefetchesIssued dvrPrefetchesCompleted" \
    "dvrDependentPrefetchesIssued dvrDependentPrefetchesCompleted"; do
    lhs="${pair%% *}"
    rhs="${pair##* }"
    equal "full_$lhs=$rhs" \
        "$(read_stat "$full_stats" "system.cpu.$lhs")" \
        "$(read_stat "$full_stats" "system.cpu.$rhs")"
done
[[ "$(read_stat "$full_stats" system.cpu.dvrVIRContinuationMaxGroupWidth)" -ge 2 ]] || {
    printf 'error: full vector did not form a multi-lane PC group\n' >&2
    exit 1
}
equal full_active_mask_failures 0 \
    "$(read_stat "$full_stats" system.cpu.dvrVIRActiveMaskFailures)"
equal full_vector_fu_conflicts 0 \
    "$(read_stat "$full_stats" system.cpu.dvrVectorFUConflictCycles)"

full_trace="${CASE_DIRS[full-vector]}/trace"
source_trace_count="$(awk -F, '$2 == "source_lane" {++n} END {print n + 0}' \
    "$full_trace/vectorization.csv")"
equal full_source_trace_count \
    "$(read_stat "$full_stats" system.cpu.dvrVectorizerSourceLanes)" \
    "$source_trace_count"
vir_trace_count="$(awk -F, '$2 == "vir_issue_group" {++n} END {print n + 0}' \
    "$full_trace/vectorization.csv")"
equal full_vir_trace_count \
    "$(read_stat "$full_stats" system.cpu.dvrHelperDynUopsDecoded)" \
    "$vir_trace_count"
vir_trace_max="$(awk -F, '$2 == "vir_issue_group" && $5 > max {max = $5} \
    END {print max + 0}' "$full_trace/vectorization.csv")"
[[ "$vir_trace_max" -le 8 ]] || {
    printf 'error: VIR trace group exceeds 512-bit width: %s lanes\n' "$vir_trace_max" >&2
    exit 1
}
target_trace_count="$(awk -F, '$2 == "replay_target" {++n} END {print n + 0}' \
    "$full_trace/dependency_chain.csv")"
equal full_replay_target_trace_count \
    "$(read_stat "$full_stats" system.cpu.dvrVectorizerDependentLanes)" \
    "$target_trace_count"
awk -F, '
    $2 == "source_value" { source[$3 SUBSEP $7] = $1; next }
    $2 == "replay_target" {
        key = $3 SUBSEP $7
        ++targets
        if (!(key in source) || source[key] > $1)
            ++bad
    }
    END {
        if (bad != 0) {
            printf "error: %d replay targets lack a prior source value\n", bad > "/dev/stderr"
            exit 1
        }
    }
' "$full_trace/dependency_chain.csv"
awk -F, '
    $2 == "source_lane" {
        ++lanes
        if (($6 + 0) < 0 || ($6 + 0) >= 128 || ($4 + 0) % 8 != 0)
            ++bad
    }
    END {
        if (bad != 0) {
            printf "error: %d source lanes violate Camel lane/address bounds\n", bad > "/dev/stderr"
            exit 1
        }
    }
' "$full_trace/vectorization.csv"

run_and_check_result nested-vector --dvr --dvr-mode=nested --dvr-vector-chunks
nested_stats="${CASE_DIRS[nested-vector]}/stats.txt"
positive nested_vector_programs "$(read_stat "$nested_stats" system.cpu.dvrVectorProgramsBuilt)"
equal nested_flatten_batches 0 \
    "$(read_stat "$nested_stats" system.cpu.dvrNestedFlattenBatches)"

run_and_check_result full-vector-unlimited --dvr --dvr-mode=full \
    --dvr-vector-chunks --dvr-unlimited-vector-fu
unlimited_stats="${CASE_DIRS[full-vector-unlimited]}/stats.txt"
equal unlimited_result "$baseline_result" \
    "$(awk '/^Result / {print $2; exit}' "${CASE_DIRS[full-vector-unlimited]}/stdout.log")"
equal unlimited_dyn_uops \
    "$(read_stat "$full_stats" system.cpu.dvrHelperDynUopsIssued)" \
    "$(read_stat "$unlimited_stats" system.cpu.dvrHelperDynUopsIssued)"
equal unlimited_prefetches \
    "$(read_stat "$full_stats" system.cpu.dvrDependentPrefetchesIssued)" \
    "$(read_stat "$unlimited_stats" system.cpu.dvrDependentPrefetchesIssued)"

baseline_ticks="$(read_stat "$baseline_stats" simTicks)"
full_ticks="$(read_stat "$full_stats" simTicks)"
speedup="$(awk -v b="$baseline_ticks" -v f="$full_ticks" \
    'BEGIN {if (f == 0) print "0.000000"; else printf "%.6f", b / f}')"
printf 'DVR_CAMEL_ARCH_CHECK_PASSED run=%s result=%s baseline_ticks=%s full_ticks=%s speedup=%sx\n' \
    "$RUN_ID" "$baseline_result" "$baseline_ticks" "$full_ticks" "$speedup"
printf 'baseline_dir=%s\nfull_vector_dir=%s\ntrace_dir=%s\n' \
    "$baseline" "${CASE_DIRS[full-vector]}" "${CASE_DIRS[full-vector]}/trace"
