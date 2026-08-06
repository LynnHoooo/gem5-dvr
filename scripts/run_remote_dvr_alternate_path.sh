#!/usr/bin/env bash
set -euo pipefail

# Run the persistent alternate-path checks on one or more binaries.
#
# A single workload is still supported through BENCH.  For a cross-workload
# observation run, pass absolute paths or names relative to BENCH_ROOT:
#
#   BENCHES=/path/dvr_divergent.riscv,/path/bfs.riscv,/path/camel.riscv \
#   OUT_ROOT=/path/to/results scripts/run_remote_dvr_alternate_path.sh
#
# dvr_divergent is the strict functional gate.  BFS and Camel are reported in
# the same CSV, but are not required to manufacture an alternate path: a zero
# is an observation about workload coverage, not a failed implementation test.
#
# For the real serial GAPBS binary, use for example:
#   BENCHES=/path/dvr_divergent.riscv,/path/gapbs/bfs,/path/camel.riscv \
#   BFS_OPTIONS='-g 11 -n 1' REQUIRE_BFS_ALTERNATE=1 \
#   scripts/run_remote_dvr_alternate_path.sh

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-alternate-path}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"
BFS_OPTIONS="${BFS_OPTIONS:--g 11 -n 1}"
REQUIRE_BFS_ALTERNATE="${REQUIRE_BFS_ALTERNATE:-0}"
REQUIRE_CAMEL_ALTERNATE="${REQUIRE_CAMEL_ALTERNATE:-0}"

if [[ -n "${BENCHES:-}" ]]; then
    BENCH_SPEC="$BENCHES"
else
    BENCH_SPEC="${BENCH:-$BENCH_ROOT/dvr_divergent.riscv}"
fi

read_stat() {
    awk -v name="$2" '$1 == name { print $2; found = 1; exit } END { if (!found) print 0 }' "$1"
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

resolve_bench() {
    local spec="$1"
    if [[ -x "$spec" ]]; then
        printf '%s\n' "$spec"
    elif [[ -x "$BENCH_ROOT/$spec" ]]; then
        printf '%s\n' "$BENCH_ROOT/$spec"
    elif [[ -x "$BENCH_ROOT/$spec.riscv" ]]; then
        printf '%s\n' "$BENCH_ROOT/$spec.riscv"
    else
        printf 'error: missing benchmark: %s\n' "$spec" >&2
        exit 1
    fi
}

[[ -x "$GEM5" ]] || { printf 'error: missing gem5: %s\n' "$GEM5" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { printf 'error: missing config: %s\n' "$CONFIG" >&2; exit 1; }

IFS=',' read -r -a bench_specs <<< "$BENCH_SPEC"
(( ${#bench_specs[@]} > 0 ))

OUT="$OUT_ROOT/$RUN_ID"
mkdir -p "$OUT"

csv="$OUT/alternate_path.csv"
printf '%s\n' \
    'workload,benchmark,baseline_committed,full_committed,complete_hits,alternate_uops,alternate_targets,demand_covered,reconvergence_resumes,stack_overflows,helper_decoded,helper_issued,helper_completed,max_group_width,trace_uops,trace_targets,trace_single_lane,trace_partial_chunk,trace_lane_sum,untraced_uops,status' \
    >"$csv"

strict_seen=0

run_workload() {
    local bench="$1"
    local name="$(basename "$bench")"
    name="${name%.riscv}"
    local workload_out="$OUT/$name"
    local baseline="$workload_out/baseline"
    local full="$workload_out/full"
    local trace="$full/trace"
    local options=()
    case "$name" in
        bfs) options=(--options="$BFS_OPTIONS") ;;
    esac

    mkdir -p "$baseline" "$trace"
    "$GEM5" --outdir="$baseline" "$CONFIG" --cmd="$bench" \
        "${options[@]}" \
        >"$baseline/stdout.log" 2>&1
    DVR_TRACE_DIR="$trace" "$GEM5" --outdir="$full" "$CONFIG" --cmd="$bench" \
        --dvr --dvr-mode=full --dvr-vector-chunks \
        --dvr-no-bounded-fallback \
        "${options[@]}" \
        >"$full/stdout.log" 2>&1

    [[ -s "$baseline/stats.txt" && -s "$full/stats.txt" ]] || {
        printf 'error: baseline/full stats are missing for %s\n' "$name" >&2
        exit 1
    }

    local baseline_committed full_committed stats
    baseline_committed="$(read_stat "$baseline/stats.txt" system.cpu.committedInsts)"
    full_committed="$(read_stat "$full/stats.txt" system.cpu.committedInsts)"
    equal "$name committed_instructions" "$full_committed" "$baseline_committed"
    case "$name" in
        bfs)
            grep -q 'BFS Tree has' "$full/stdout.log" || {
                printf 'error: %s did not complete BFS\n' "$name" >&2
                exit 1
            }
            ;;
        camel)
            grep -q '^Result ' "$full/stdout.log" || {
                printf 'error: %s did not complete Camel\n' "$name" >&2
                exit 1
            }
            ;;
    esac
    stats="$full/stats.txt"

    local complete_hits alternate_uops alternate_targets alternate_covered
    local alternate_resumes stack_overflows helper_decoded helper_issued helper_completed
    local max_group
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

    equal "$name reconvergence_stack_overflows" "$stack_overflows" 0
    equal "$name helper_dyn_uops_issued" "$helper_issued" "$helper_decoded"
    equal "$name helper_dyn_uops_completed" "$helper_completed" "$helper_issued"
    [[ "$alternate_covered" -le "$alternate_targets" ]] || {
        printf 'error: %s demand coverage exceeds alternate target generation\n' "$name" >&2
        exit 1
    }

    local trace_alt_uops trace_alt_single trace_alt_partial trace_alt_lane_sum trace_alt_targets
    trace_alt_uops=0
    trace_alt_single=0
    trace_alt_partial=0
    trace_alt_lane_sum=0
    trace_alt_targets=0
    if [[ -s "$trace/vectorization.csv" ]]; then
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
            ' "$trace/vectorization.csv")
    fi
    if [[ -s "$trace/dependency_chain.csv" ]]; then
        trace_alt_targets="$(awk -F, '$2 == "alternate_replay_target" { count++ } END { print count + 0 }' "$trace/dependency_chain.csv")"
    fi

    [[ "$trace_alt_lane_sum" -le "$alternate_uops" ]] || {
        printf 'error: %s persistent trace exceeds alternate uop total\n' "$name" >&2
        exit 1
    }
    equal "$name trace_alternate_targets" "$trace_alt_targets" "$alternate_targets"
    local untraced_alt_uops=$((alternate_uops - trace_alt_lane_sum))
    local status="observed"

    # This is the functional gate. Other workloads are deliberately only
    # observed because their control-flow shape may not expose this path.
    if [[ "$name" == "dvr_divergent" || "${STRICT_WORKLOAD:-}" == "$name" ]]; then
        strict_seen=1
        positive "$name alternate_path_complete_hits" "$complete_hits"
        positive "$name alternate_path_uops_replayed" "$alternate_uops"
        positive "$name alternate_path_dependent_targets" "$alternate_targets"
        positive "$name alternate_path_demand_covered" "$alternate_covered"
        positive "$name alternate_path_reconvergence_resumes" "$alternate_resumes"
        positive "$name same_pc_group_width" "$max_group"
        positive "$name trace_alternate_path_uops" "$trace_alt_uops"
        positive "$name trace_single_lane_alternate_uops" "$trace_alt_single"
        positive "$name trace_partial_chunk_alternate_uops" "$trace_alt_partial"
        status="strict_pass"
    elif [[ "$name" == "bfs" && "$REQUIRE_BFS_ALTERNATE" == 1 ]]; then
        # A real BFS run is allowed to terminate an alternate lane at its FLR:
        # target generation proves cache-to-lane admission; demand coverage
        # and reconvergence are reported independently rather than required.
        positive "$name alternate_path_complete_hits" "$complete_hits"
        positive "$name alternate_path_uops_replayed" "$alternate_uops"
        positive "$name alternate_path_dependent_targets" "$alternate_targets"
        status="bfs_alternate_pass"
    elif [[ "$name" == "camel" && "$REQUIRE_CAMEL_ALTERNATE" == 1 ]]; then
        positive "$name alternate_path_complete_hits" "$complete_hits"
        positive "$name alternate_path_uops_replayed" "$alternate_uops"
        positive "$name alternate_path_dependent_targets" "$alternate_targets"
        status="camel_alternate_pass"
    elif [[ "$alternate_uops" -gt 0 || "$alternate_targets" -gt 0 ||
            "$alternate_covered" -gt 0 ]]; then
        status="alternate_observed"
    elif [[ "$complete_hits" -gt 0 ]]; then
        status="cache_hit_only"
    else
        status="no_alternate_path_observed"
    fi

    printf '%s\n' \
        "$name,$bench,$baseline_committed,$full_committed,$complete_hits,$alternate_uops,$alternate_targets,$alternate_covered,$alternate_resumes,$stack_overflows,$helper_decoded,$helper_issued,$helper_completed,$max_group,$trace_alt_uops,$trace_alt_targets,$trace_alt_single,$trace_alt_partial,$trace_alt_lane_sum,$untraced_alt_uops,$status" \
        >>"$csv"
    printf 'workload=%s status=%s committed=%s complete_hits=%s alternate_uops=%s alternate_targets=%s demand_covered=%s resumes=%s trace_uops=%s trace_targets=%s max_group=%s\n' \
        "$name" "$status" "$full_committed" "$complete_hits" "$alternate_uops" \
        "$alternate_targets" "$alternate_covered" "$alternate_resumes" \
        "$trace_alt_uops" "$trace_alt_targets" "$max_group"
}

for spec in "${bench_specs[@]}"; do
    bench="$(resolve_bench "$spec")"
    run_workload "$bench"
done

[[ "$strict_seen" -eq 1 ]] || {
    printf 'error: no strict workload found; include dvr_divergent.riscv or set STRICT_WORKLOAD\n' >&2
    exit 1
}

cat "$csv"
printf 'DVR_ALTERNATE_PATH_MULTI_PASSED run=%s output=%s summary=%s\n' "$RUN_ID" "$OUT" "$csv"
