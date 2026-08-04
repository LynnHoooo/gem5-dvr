#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results/dvr-stage1-smoke}"
PYTHON_ROOT="${PYTHON_ROOT:-/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_stride.riscv}"

export LD_LIBRARY_PATH="$PYTHON_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$RESULT_ROOT"

gem5="$GEM5"
binary="$BENCH"
test -x "$gem5"
test -x "$binary"

"$gem5" -d "$RESULT_ROOT" "$ROOT/configs/example/se.py" \
    --cpu-type=DerivO3CPU --caches --cmd="$binary" \
    --param 'system.cpu[0].enableDVR=True' \
    --param 'system.cpu[0].dvrRPTEntries=32' \
    --param 'system.cpu[0].dvrMaxLanes=128' \
    >"$RESULT_ROOT/run.log" 2>&1

grep -q '^enableDVR=true$' "$RESULT_ROOT/config.ini"
grep -q 'exiting with last active thread context' "$RESULT_ROOT/run.log"
loads=$(awk '$1 ~ /dvrLoadsObserved$/ {print $2}' "$RESULT_ROOT/stats.txt" | tail -1)
candidates=$(awk '$1 ~ /dvrStrideCandidates$/ {print $2}' "$RESULT_ROOT/stats.txt" | tail -1)
test "${loads:-0}" -gt 0
test "${candidates:-0}" -gt 0

printf 'DVR_STAGE1_SMOKE_PASSED loads=%s candidates=%s\n' "$loads" "$candidates"
