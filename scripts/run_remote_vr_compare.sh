#!/usr/bin/env bash
set -euo pipefail

# Fixed-workload OoO/PRE/VR comparison.  The same binary, cache hierarchy,
# command line and output parser are used for all three configurations.
ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results/vr-compare}"
BINARY="${BINARY:-$ROOT/benchmarks/vr_indirect.riscv}"

test -x "$GEM5"
test -x "$BINARY"
mkdir -p "$RESULT_ROOT"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

run_case() {
    local name="$1"
    shift
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$GEM5" -d "$out" "$ROOT/configs/example/se.py" \
        --cpu-type=DerivO3CPU --caches --cmd="$BINARY" "$@" \
        >"$out/run.log" 2>&1
    grep -q 'exiting with last active thread context' "$out/run.log"
}

run_case ooo
run_case pre --param 'system.cpu[0].enablePRE=True'
run_case vr --param 'system.cpu[0].enablePRE=True' \
           --param 'system.cpu[0].enableVR=True'

base_cycles="$(read_stat "$RESULT_ROOT/ooo/stats.txt" system.cpu.numCycles)"
pre_cycles="$(read_stat "$RESULT_ROOT/pre/stats.txt" system.cpu.numCycles)"
vr_cycles="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.numCycles)"
vr_rounds="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrRoundsEntered)"
vr_gathers="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrGathersIssued)"
vr_issued="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrPrefetchesIssued)"
vr_completed="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrPrefetchesCompleted)"
vr_dependent="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrDependentPrefetchesGenerated)"
vr_retries="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrPrefetchRetries)"
vr_vrat="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrVRATAllocations)"
vr_rdq_enq="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrRDQEnqueues)"
vr_rdq_rel="$(read_stat "$RESULT_ROOT/vr/stats.txt" system.cpu.vrRDQReleases)"

python3 - "$base_cycles" "$pre_cycles" "$vr_cycles" <<'PY'
import sys
b, p, v = map(float, sys.argv[1:])
print("VR_COMPARE cycles_ooo=%g cycles_pre=%g cycles_vr=%g pre_speedup=%.6f vr_speedup=%.6f vr_vs_pre=%.6f" %
      (b, p, v, b / p, b / v, p / v))
PY
printf 'VR_ACTIVITY rounds=%s gathers=%s issued=%s completed=%s dependent_generated=%s retries=%s vrat_allocations=%s rdq_enqueues=%s rdq_releases=%s\n' \
    "$vr_rounds" "$vr_gathers" "$vr_issued" "$vr_completed" \
    "$vr_dependent" "$vr_retries" "$vr_vrat" "$vr_rdq_enq" "$vr_rdq_rel"
if [[ "${vr_dependent:-0}" -le 0 ]]; then
    printf 'VR_COMPARE_INCOMPLETE dependent_chain_not_observed\n' >&2
fi
