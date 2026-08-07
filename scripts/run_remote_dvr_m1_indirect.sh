#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
SOURCE="${SOURCE:-$REPO_ROOT/benchmarks/dvr_single_indirect.c}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-m1-single-indirect}"
CC="${CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"
BENCH="$OUT_ROOT/dvr_single_indirect.riscv"

read_stat() { awk -v name="$2" '$1 == name {print $2; exit}' "$1"; }
[[ -x "$GEM5" && -f "$CONFIG" && -f "$SOURCE" && -x "$CC" ]]
mkdir -p "$OUT_ROOT/baseline" "$OUT_ROOT/full"
"$CC" -O2 -fno-tree-vectorize -fno-unroll-loops -nostdlib -static \
    -march=rv64gc -mabi=lp64d -o "$BENCH" "$SOURCE"
"$GEM5" --outdir="$OUT_ROOT/baseline" "$CONFIG" --cmd="$BENCH" \
    >"$OUT_ROOT/baseline/stdout.log" 2>&1
"$GEM5" --outdir="$OUT_ROOT/full" "$CONFIG" --cmd="$BENCH" --dvr \
    --dvr-mode=full --dvr-vector-chunks \
    >"$OUT_ROOT/full/stdout.log" 2>&1
stats="$OUT_ROOT/full/stats.txt"
base_committed="$(read_stat "$OUT_ROOT/baseline/stats.txt" system.cpu.committedInsts)"
full_committed="$(read_stat "$stats" system.cpu.committedInsts)"
targets="$(read_stat "$stats" system.cpu.dvrReplayTargetsGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesCompleted)"
demands="$(read_stat "$stats" system.cpu.dvrDependentDemandLoads)"
covered="$(read_stat "$stats" system.cpu.dvrDependentDemandCovered)"
faults="$(read_stat "$stats" system.cpu.dvrPrefetchTranslationFaults)"
max_width="$(read_stat "$stats" system.cpu.dvrVIRContinuationMaxGroupWidth)"
[[ "$base_committed" -eq "$full_committed" ]]
[[ "${targets:-0}" -gt 0 && "${issued:-0}" -gt 0 && "$issued" -eq "$completed" ]]
[[ "${demands:-0}" -gt 0 && "${covered:-0}" -gt 0 ]]
awk -v covered="$covered" -v demands="$demands" 'BEGIN { exit !((covered / demands) > 0.80) }'
[[ "${max_width:-0}" -gt 1 && "${faults:-0}" -eq 0 ]]
printf 'DVR_M1_INDIRECT_PASSED committed=%s targets=%s dependent=%s/%s coverage=%s/%s max_width=%s faults=%s out=%s\n' \
    "$full_committed" "$targets" "$issued" "$completed" "$covered" \
    "$demands" "$max_width" "$faults" "$OUT_ROOT"
