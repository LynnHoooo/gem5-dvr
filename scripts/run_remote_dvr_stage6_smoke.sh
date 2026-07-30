#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage6-lane-count}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
matches="$(read_stat "$stats" system.cpu.dvrLoopBoundMatches)"
fallbacks="$(read_stat "$stats" system.cpu.dvrLoopBoundFallbacks)"
samples="$(read_stat "$stats" system.cpu.dvrLaneCountSamples)"
lanes="$(read_stat "$stats" system.cpu.dvrTotalActiveLanes)"
test -n "$matches" && test "$matches" -gt 0
test -n "$samples" && test "$samples" -gt 0
test -n "$lanes" && test "$lanes" -gt 0
test $((matches + fallbacks)) -eq "$samples"

printf 'DVR_STAGE6_SMOKE_PASSED matches=%s fallbacks=%s samples=%s lanes=%s\n' \
    "$matches" "$fallbacks" "$samples" "$lanes"
