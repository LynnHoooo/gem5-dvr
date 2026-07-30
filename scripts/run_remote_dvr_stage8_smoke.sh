#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage8-dependent-prefetch}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
relations="$(read_stat "$stats" system.cpu.dvrAddressRelationsTrained)"
generated="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesCompleted)"
test -n "$relations" && test "$relations" -gt 0
test -n "$generated" && test "$generated" -gt 0
test -n "$issued" && test "$issued" -gt 0
test -n "$completed" && test "$completed" -gt 0
test "$completed" -le "$issued"

printf 'DVR_STAGE8_SMOKE_PASSED relations=%s generated=%s issued=%s completed=%s\n' \
    "$relations" "$generated" "$issued" "$completed"
