#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage5-loop-bound}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
backward="$(read_stat "$stats" system.cpu.dvrBackwardBranches)"
bounds="$(read_stat "$stats" system.cpu.dvrLoopBoundsFound)"
discoveries="$(read_stat "$stats" system.cpu.dvrDiscoveriesWithBounds)"
test -n "$backward" && test "$backward" -gt 0
test -n "$bounds" && test "$bounds" -gt 0
test -n "$discoveries" && test "$discoveries" -gt 0

printf 'DVR_STAGE5_SMOKE_PASSED backward=%s bounds=%s discoveries=%s\n' \
    "$backward" "$bounds" "$discoveries"
