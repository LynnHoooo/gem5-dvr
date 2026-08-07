#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
BENCH_ROOT="${BENCH_ROOT:-/home/lynnhoo/gem-test/gem5-leap/leap-bench}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/final}"
RUN_ID="${RUN_ID:-$(date +%Y%m%dT%H%M%S)-resource-$$}"
CAMEL_MAX_KEY="${CAMEL_MAX_KEY:-1024}"
CC="${CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"

read_stat() { awk -v name="$2" '$1 == name { print $2; found = 1; exit } END { if (!found) print 0 }' "$1"; }
[[ -x "$GEM5" && -f "$CONFIG" && -x "$CC" ]] || { echo "missing gem5/config/compiler" >&2; exit 1; }

OUT="$OUT_ROOT/$RUN_ID"
mkdir -p "$OUT/benchmarks"
"$CC" -g -O3 -static -march=rv64gc -mcmodel=medany -DMAX_KEY="$CAMEL_MAX_KEY" \
    -o "$OUT/benchmarks/camel.riscv" "$BENCH_ROOT/hpc/camel/camel.c" -lm
CMD="$OUT/benchmarks/camel.riscv"

run_case() {
    local name="$1"; shift
    local dir="$OUT/$name"
    mkdir -p "$dir"
    "$GEM5" --outdir="$dir" "$CONFIG" --cmd="$CMD" --options="" \
        --dvr-quality-probe --dvr --dvr-mode=full --dvr-vector-chunks "$@" \
        >"$dir/stdout.log" 2>&1
    [[ -s "$dir/stats.txt" ]] || { echo "missing stats for $name" >&2; exit 1; }
}

BASE="$OUT/Baseline"
mkdir -p "$BASE"
"$GEM5" --outdir="$BASE" "$CONFIG" --cmd="$CMD" --options="" --dvr-quality-probe \
    >"$BASE/stdout.log" 2>&1
base_ipc="$(read_stat "$BASE/stats.txt" system.cpu.ipc)"

printf '%s\n' 'experiment,value,sim_ticks,ipc,normalized_ipc,loop_bound_matches,vector_programs,replay_targets,dependent_issued,dependent_completed,translation_faults,fu_conflict_cycles,vector_latency_cycles' >"$OUT/resource_sensitivity.csv"
record() {
    local name="$1" label="$2" dir="$OUT/$name" ipc norm
    ipc="$(read_stat "$dir/stats.txt" system.cpu.ipc)"
    norm="$(awk -v x="$ipc" -v b="$base_ipc" 'BEGIN { if (b != 0) printf "%.6f", x / b; else print 0 }')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$label" "$name" "$(read_stat "$dir/stats.txt" simTicks)" "$ipc" "$norm" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrLoopBoundMatches)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrVectorProgramsBuilt)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrReplayTargetsGenerated)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrDependentPrefetchesIssued)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrDependentPrefetchesCompleted)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrPrefetchTranslationFaults)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrVectorFUConflictCycles)" \
        "$(read_stat "$dir/stats.txt" system.cpu.dvrVectorLatencyCycles)" >>"$OUT/resource_sensitivity.csv"
}

for uops in 8 16 32 64; do
    name="maxuops-$uops"
    run_case "$name" --dvr-helper-max-uops="$uops"
    record "$name" "MaxUops=$uops"
done
for variant in constrained unlimited; do
    name="fu-$variant"
    args=()
    [[ "$variant" == unlimited ]] && args+=(--dvr-unlimited-vector-fu)
    run_case "$name" "${args[@]}"
    record "$name" "VectorFU=$variant"
done
for bits in 32 64; do
    name="element-$bits"
    run_case "$name" --dvr-vector-element-bits="$bits"
    record "$name" "ElementBits=$bits"
done
for lanes in 32 64 128; do
    name="lanes-$lanes"
    run_case "$name" --dvr-max-lanes="$lanes"
    record "$name" "MaxLanes=$lanes"
done

cat >>"$OUT/resource_sensitivity.csv" <<'EOF'
VIRCopies,not_exposed,,,,,,,,,,,
HelperFrontendWidth,not_exposed,,,,,,,,,,,
NDMOuterInvocations,implementation_cap_16_no_cli_override,,,,,,,,,,,
EOF
cat >"$OUT/resource_manifest.txt" <<EOF
git_sha=$(git -C "$REPO_ROOT" rev-parse HEAD)
workload=camel
benchmark_root=$BENCH_ROOT
camel_max_key=$CAMEL_MAX_KEY
baseline_ipc=$base_ipc
available_knobs=MaxUops(8,16,32,64),VectorFU(constrained,unlimited),ElementBits(32,64),MaxLanes(32,64,128)
unexposed_knobs=VIRCopies,HelperFrontendWidth
ndm_outer_cap=16
EOF
printf 'DVR_RESOURCE_SENSITIVITY_COMPLETE out=%s csv=%s\n' "$OUT" "$OUT/resource_sensitivity.csv"
