#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage7-prefetch}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
generated="$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.dvrPrefetchesCompleted)"
dropped="$(read_stat "$stats" system.cpu.dvrPrefetchesDropped)"
faults="$(read_stat "$stats" system.cpu.dvrPrefetchTranslationFaults)"
test -n "$generated" && test "$generated" -gt 0
test -n "$issued" && test "$issued" -gt 0
test -n "$completed" && test "$completed" -gt 0
test "$completed" -le "$issued"

printf 'DVR_STAGE7_SMOKE_PASSED generated=%s issued=%s completed=%s dropped=%s faults=%s\n' \
    "$generated" "$issued" "$completed" "$dropped" "$faults"
