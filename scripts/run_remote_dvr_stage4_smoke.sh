#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage4-taint-flr}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
tainted="$(read_stat "$stats" system.cpu.dvrTaintedInstructions)"
loads="$(read_stat "$stats" system.cpu.dvrDependentLoads)"
flr="$(read_stat "$stats" system.cpu.dvrDiscoveriesWithFLR)"
test -n "$tainted" && test "$tainted" -gt 0
test -n "$loads" && test "$loads" -gt 0
test -n "$flr" && test "$flr" -gt 0

printf 'DVR_STAGE4_SMOKE_PASSED tainted=%s dependent_loads=%s with_flr=%s\n' \
    "$tainted" "$loads" "$flr"
