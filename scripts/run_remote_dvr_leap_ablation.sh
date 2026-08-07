#!/usr/bin/env bash
set -euo pipefail

# Reproducible Figure-8-style ablation on the LeAP sources supplied outside
# this repository.  The script deliberately builds direct single-process
# RISC-V ELFs, so fork-based qemu wrappers do not enter the comparison.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BENCH_ROOT="${BENCH_ROOT:-/home/lynnhoo/gem-test/gem5-leap/leap-bench}"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/final}"
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-$$}"
WORKLOADS="${WORKLOADS:-camel,bfs}"
BFS_SCALE="${BFS_SCALE:-6}"
BFS_BAREMETAL="${BFS_BAREMETAL:-1}"
CAMEL_MAX_KEY="${CAMEL_MAX_KEY:-1024}"
ORACLE_LOOKAHEAD="${ORACLE_LOOKAHEAD:-32}"
CC="${CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"
CXX="${CXX:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-g++}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
read_stat() {
    awk -v name="$2" '$1 == name { print $2; found = 1; exit }
         END { if (!found) print 0 }' "$1"
}
num_or_zero() {
    case "${1:-}" in
        ''|nan|NaN) printf '0\n' ;;
        *) printf '%s\n' "$1" ;;
    esac
}

[[ -x "$GEM5" ]] || die "missing gem5: $GEM5"
[[ -f "$CONFIG" ]] || die "missing config: $CONFIG"
[[ -d "$BENCH_ROOT" ]] || die "missing LeAP root: $BENCH_ROOT"
[[ -x "$CC" && -x "$CXX" ]] || die "missing RISC-V compiler pair"

OUT="$OUT_ROOT/$RUN_ID"
BUILD_DIR="$OUT/benchmarks"
mkdir -p "$BUILD_DIR"

"$CC" -g -O3 -static -march=rv64gc -mcmodel=medany \
    -DMAX_KEY="$CAMEL_MAX_KEY" \
    -o "$BUILD_DIR/camel.riscv" "$BENCH_ROOT/hpc/camel/camel.c" -lm

declare -A BENCH OPTIONS
BENCH[camel]="$BUILD_DIR/camel.riscv"
OPTIONS[camel]=""

IFS=',' read -r -a workload_list <<< "$WORKLOADS"
(( ${#workload_list[@]} > 0 )) || die "empty WORKLOADS"
for workload in "${workload_list[@]}"; do
    if [[ "$workload" == bfs ]]; then
        bfs_defs=(-DGAP_BFS_ALPHA=15 -DGAP_BFS_BETA=18)
        if [[ "$BFS_BAREMETAL" == 1 ]]; then
            bfs_defs+=(-DGAP_BAREMETAL)
        fi
        "$CXX" -g -std=c++11 -O3 -static -march=rv64gc -mcmodel=medany \
            -fno-tree-vectorize -I"$BENCH_ROOT/gap/src" \
            -I"$BENCH_ROOT/gap/benchmark" "${bfs_defs[@]}" \
            -o "$BUILD_DIR/bfs.riscv" "$BENCH_ROOT/gap/src/bfs.cc"
        BENCH[bfs]="$BUILD_DIR/bfs.riscv"
        OPTIONS[bfs]="-g $BFS_SCALE -n 1 -k 8 -a -v"
    elif [[ "$workload" != camel ]]; then
        die "unsupported workload: $workload"
    fi
done
for workload in "${workload_list[@]}"; do
    [[ -x "${BENCH[$workload]}" ]] || die "benchmark build failed: $workload"
done

run_case() {
    local workload="$1" mode="$2" out="$OUT/$workload/$mode"
    local trace_dir="$out/trace"
    local args=()
    mkdir -p "$out"
    case "$mode" in
        Baseline) ;;
        VR) args=(--dvr --dvr-mode=vr --dvr-vector-chunks) ;;
        Offload) args=(--dvr --dvr-mode=offload --dvr-vector-chunks) ;;
        Discovery) args=(--dvr --dvr-mode=full --dvr-vector-chunks) ;;
        Multiple) args=(--dvr --dvr-mode=nested --dvr-vector-chunks) ;;
        Oracle)
            args=(--oracle --oracle-lookahead="$ORACLE_LOOKAHEAD" \
                  --oracle-trace="$OUT/$workload/Baseline/trace/workload.csv") ;;
        *) die "unknown mode: $mode" ;;
    esac
    local trace_env=()
    # Baseline trace is required by Oracle.  Full DVR traces are retained for
    # the final evidence package; other bars only need stats.
    if [[ "$mode" == Baseline || "$mode" == Multiple ]]; then
        mkdir -p "$trace_dir"
        trace_env=(DVR_TRACE_DIR="$trace_dir")
    fi
    if ((${#trace_env[@]})); then
        DVR_TRACE_DIR="$trace_dir" "$GEM5" --outdir="$out" "$CONFIG" \
            --cmd="${BENCH[$workload]}" --options="${OPTIONS[$workload]}" \
            --dvr-quality-probe "${args[@]}" >"$out/stdout.log" 2>&1
    else
        "$GEM5" --outdir="$out" "$CONFIG" \
            --cmd="${BENCH[$workload]}" --options="${OPTIONS[$workload]}" \
            --dvr-quality-probe "${args[@]}" >"$out/stdout.log" 2>&1
    fi
    [[ -s "$out/stats.txt" ]] || die "missing stats: $workload/$mode"
    if [[ "$workload" == bfs ]]; then
        grep -q 'BFS Tree has' "$out/stdout.log" || die "BFS did not finish: $mode"
        ! grep -Eq 'Source wrong|Wrong depths|Couldn.t find edge|Reachability mismatch' \
            "$out/stdout.log" || die "BFS verifier failed: $mode"
    else
        grep -q '^Result ' "$out/stdout.log" || die "Camel did not finish: $mode"
    fi
}

for workload in "${workload_list[@]}"; do
    for mode in Baseline VR Offload Discovery Multiple Oracle; do
        printf 'running %s/%s\n' "$workload" "$mode"
        run_case "$workload" "$mode"
    done
done

printf '%s\n' \
    'workload,mode,sim_ticks,ipc,normalized_ipc,committed_insts,committed_match,translation_faults,helper_generated,helper_issued,helper_completed,dependent_generated,dependent_issued,dependent_completed,quality_coverage,quality_accuracy,quality_timeliness,quality_pollution_evictions' \
    >"$OUT/performance.csv"
printf '%s\n' \
    'workload,mode,stride_candidates,discovery_starts,discovery_completions,loop_bound_matches,vector_programs,source_issued,source_completed,replay_attempts,replay_targets,dependent_demand_loads,dependent_demand_covered,ndm_attempts,outer_invocations,flattened_lanes,flatten_expected,flatten_failures,alternate_hits,alternate_uops,alternate_targets,alternate_demand_covered,reconvergence_resumes' \
    >"$OUT/mechanism.csv"
printf '%s\n' \
    'workload,mode,baseline_committed,committed,committed_match,translation_faults,helper_issued,helper_completed,dependent_issued,dependent_completed,helper_pending,active_mask_failures,reconvergence_stack_overflows,status' \
    >"$OUT/correctness.csv"

for workload in "${workload_list[@]}"; do
    base_stats="$OUT/$workload/Baseline/stats.txt"
    base_committed="$(read_stat "$base_stats" system.cpu.committedInsts)"
    base_ipc="$(read_stat "$base_stats" system.cpu.ipc)"
    for mode in Baseline VR Offload Discovery Multiple Oracle; do
        stats="$OUT/$workload/$mode/stats.txt"
        committed="$(read_stat "$stats" system.cpu.committedInsts)"
        match=0; [[ "$committed" == "$base_committed" ]] && match=1
        faults="$(read_stat "$stats" system.cpu.dvrPrefetchTranslationFaults)"
        helper_generated="$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)"
        helper_issued="$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
        helper_completed="$(read_stat "$stats" system.cpu.dvrPrefetchesCompleted)"
        dep_generated="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesGenerated)"
        dep_issued="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesIssued)"
        dep_completed="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesCompleted)"
        ipc="$(read_stat "$stats" system.cpu.ipc)"
        norm="0"; [[ "${base_ipc:-0}" != 0 ]] && norm="$(awk -v x="$ipc" -v b="$base_ipc" 'BEGIN { printf "%.6f", x / b }')"
        coverage="$(num_or_zero "$(read_stat "$stats" system.cpu.dvr_quality_probe.coverage)")"
        accuracy="$(num_or_zero "$(read_stat "$stats" system.cpu.dvr_quality_probe.fillAccuracy)")"
        timeliness="$(num_or_zero "$(read_stat "$stats" system.cpu.dvr_quality_probe.timeliness)")"
        pollution="$(num_or_zero "$(read_stat "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)")"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$workload" "$mode" "$(read_stat "$stats" simTicks)" "$ipc" "$norm" \
            "$committed" "$match" "$faults" "$helper_generated" "$helper_issued" \
            "$helper_completed" "$dep_generated" "$dep_issued" "$dep_completed" \
            "$coverage" "$accuracy" "$timeliness" "$pollution" >>"$OUT/performance.csv"
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$workload" "$mode" "$(read_stat "$stats" system.cpu.dvrStrideCandidates)" \
            "$(read_stat "$stats" system.cpu.dvrDiscoveryStarts)" "$(read_stat "$stats" system.cpu.dvrDiscoveryCompletions)" \
            "$(read_stat "$stats" system.cpu.dvrLoopBoundMatches)" "$(read_stat "$stats" system.cpu.dvrVectorProgramsBuilt)" \
            "$(read_stat "$stats" system.cpu.dvrSourcePrefetchesIssued)" "$(read_stat "$stats" system.cpu.dvrSourcePrefetchesCompleted)" \
            "$(read_stat "$stats" system.cpu.dvrReplayAttempts)" "$(read_stat "$stats" system.cpu.dvrReplayTargetsGenerated)" \
            "$(read_stat "$stats" system.cpu.dvrDependentDemandLoads)" "$(read_stat "$stats" system.cpu.dvrDependentDemandCovered)" \
            "$(read_stat "$stats" system.cpu.dvrNDMAttempts)" "$(read_stat "$stats" system.cpu.dvrNDMOuterInvocations)" \
            "$(read_stat "$stats" system.cpu.dvrNestedFlattenedLanes)" "$(read_stat "$stats" system.cpu.dvrNestedFlattenExpectedLanes)" \
            "$(read_stat "$stats" system.cpu.dvrNestedFlattenInvariantFailures)" \
            "$(read_stat "$stats" system.cpu.dvrAlternatePathHits)" "$(read_stat "$stats" system.cpu.dvrAlternatePathUopsReplayed)" \
            "$(read_stat "$stats" system.cpu.dvrAlternatePathDependentTargets)" "$(read_stat "$stats" system.cpu.dvrAlternatePathDemandCovered)" \
            "$(read_stat "$stats" system.cpu.dvrReconvergenceResumeSuccesses)" >>"$OUT/mechanism.csv"
        status=pass
        [[ "$match" == 1 && "$faults" == 0 && "$helper_issued" == "$helper_completed" && \
           "$dep_issued" == "$dep_completed" ]] || status=observe
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$workload" "$mode" "$base_committed" "$committed" "$match" "$faults" \
            "$helper_issued" "$helper_completed" "$dep_issued" "$dep_completed" \
            "$(read_stat "$stats" system.cpu.dvrHelperLoadEntryPending)" \
            "$(read_stat "$stats" system.cpu.dvrVIRActiveMaskFailures)" \
            "$(read_stat "$stats" system.cpu.dvrReconvergenceStackOverflows)" "$status" \
            >>"$OUT/correctness.csv"
    done
done

printf 'workload,mode,harmonic_normalized_ipc\n' >"$OUT/harmonic_mean.csv"
for mode in Baseline VR Offload Discovery Multiple Oracle; do
    awk -F, -v mode="$mode" '$2 == mode {sum += 1 / $5; n++} END { if (n) printf "%s,%s,%.6f\n", "H-mean", mode, n / sum }' \
        "$OUT/performance.csv" >>"$OUT/harmonic_mean.csv"
done

cat >"$OUT/manifest.txt" <<EOF
git_sha=$(git -C "$REPO_ROOT" rev-parse HEAD)
gem5=$GEM5
gem5_sha256=$(sha256sum "$GEM5" | awk '{print $1}')
benchmark_root=$BENCH_ROOT
bfs_scale=$BFS_SCALE
camel_max_key=$CAMEL_MAX_KEY
oracle_lookahead=$ORACLE_LOOKAHEAD
workloads=$WORKLOADS
EOF

printf 'DVR_LEAP_ABLATION_COMPLETE out=%s performance=%s mechanism=%s correctness=%s\n' \
    "$OUT" "$OUT/performance.csv" "$OUT/mechanism.csv" "$OUT/correctness.csv"
