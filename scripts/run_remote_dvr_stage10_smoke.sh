#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage10-vrat-vir}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
uops="$(read_stat "$stats" system.cpu.dvrRecordedUops)"
overflows="$(read_stat "$stats" system.cpu.dvrRecorderOverflows)"
programs="$(read_stat "$stats" system.cpu.dvrVectorProgramsBuilt)"
allocations="$(read_stat "$stats" system.cpu.dvrVRATAllocations)"
issues="$(read_stat "$stats" system.cpu.dvrVIRChunkIssues)"
executions="$(read_stat "$stats" system.cpu.dvrVIRChunkExecutions)"
test -n "$uops" && test "$uops" -gt 0
test -n "$programs" && test "$programs" -gt 0
test -n "$allocations" && test "$allocations" -gt 0
test -n "$issues" && test "$issues" -gt 0
test "$executions" -eq "$issues"

printf 'DVR_STAGE10_SMOKE_PASSED uops=%s overflows=%s programs=%s vrat_allocations=%s vir_issues=%s vir_executions=%s\n' \
    "$uops" "$overflows" "$programs" "$allocations" "$issues" \
    "$executions"
