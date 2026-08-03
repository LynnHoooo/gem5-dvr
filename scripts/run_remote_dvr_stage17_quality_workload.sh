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
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage17-quality-workload}"

read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }

test -x "$GEM5"
test -x "$BENCH"
rm -rf "$OUT"
mkdir -p "$OUT"
"$GEM5" --outdir="$OUT" "$CONFIG" --cmd="$BENCH" --dvr \
    --dvr-quality-probe

stats="$OUT/stats.txt"
demand="$(read_stat "$stats" system.cpu.dvr_quality_probe.demandAccesses)"
issued="$(read_stat "$stats" system.cpu.dvr_quality_probe.issued)"
completed="$(read_stat "$stats" system.cpu.dvr_quality_probe.completed)"
fills="$(read_stat "$stats" system.cpu.dvr_quality_probe.fills)"
timely="$(read_stat "$stats" system.cpu.dvr_quality_probe.usefulTimely)"
late="$(read_stat "$stats" system.cpu.dvr_quality_probe.usefulLate)"
coverage="$(read_stat "$stats" system.cpu.dvr_quality_probe.coverage)"
timeliness="$(read_stat "$stats" system.cpu.dvr_quality_probe.timeliness)"

test -n "$demand"
test -n "$issued"
test -n "$completed"
test -n "$fills"
test -n "$timely"
test -n "$late"
test -n "$coverage"
test -n "$timeliness"
test "$demand" -gt 0
test "$issued" -gt 0
test "$completed" -gt 0
test "$fills" -gt 0
test "$timely" -gt 0
test "$completed" -le "$issued"

printf 'DVR_STAGE17_QUALITY_WORKLOAD_PASSED demand=%s issued=%s completed=%s fills=%s timely=%s late=%s coverage=%s timeliness=%s\n' \
    "$demand" "$issued" "$completed" "$fills" "$timely" "$late" \
    "$coverage" "$timeliness"
