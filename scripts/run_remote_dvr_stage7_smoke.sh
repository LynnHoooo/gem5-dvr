#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
if [[ ! -d "$BENCH_ROOT" && -d "$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks" ]]; then
    BENCH_ROOT="$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks"
fi
BENCH="${BENCH:-$BENCH_ROOT/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage7-prefetch}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$GEM5" --outdir="$OUT" \
    "$CONFIG" --cmd="$BENCH" --dvr \
    --dvr-no-dependent-prefetch

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
generated="$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.dvrPrefetchesCompleted)"
dropped="$(read_stat "$stats" system.cpu.dvrPrefetchesDropped)"
faults="$(read_stat "$stats" system.cpu.dvrPrefetchTranslationFaults)"
source_faults="$(read_stat "$stats" system.cpu.dvrSourcePrefetchTranslationFaults)"
dependent_faults="$(read_stat "$stats" system.cpu.dvrDependentPrefetchTranslationFaults)"
test -n "$generated" && test "$generated" -gt 0
test -n "$issued" && test "$issued" -gt 0
test -n "$completed" && test "$completed" -gt 0
test "$completed" -le "$issued"
test -n "$source_faults" && test "$source_faults" -eq 0
test -n "$dependent_faults" && test "$dependent_faults" -eq 0
test "$faults" -eq 0

printf 'DVR_STAGE7_SMOKE_PASSED generated=%s issued=%s completed=%s dropped=%s faults=%s source_faults=%s dependent_faults=%s\n' \
    "$generated" "$issued" "$completed" "$dropped" "$faults" \
    "$source_faults" "$dependent_faults"
