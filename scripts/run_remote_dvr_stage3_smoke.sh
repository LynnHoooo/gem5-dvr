#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_stride.riscv}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"

read_stat() {
    local file="$1"
    local name="$2"
    awk -v name="$name" '$1 == name { print $2; exit }' "$file"
}

run_case() {
    local name="$1"
    local limit="$2"
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr \
        --discovery-max-insts="$limit"
}

run_case dvr-stage3-complete 512
complete_stats="$RESULT_ROOT/dvr-stage3-complete/stats.txt"
starts="$(read_stat "$complete_stats" system.cpu.dvrDiscoveryStarts)"
completions="$(read_stat "$complete_stats" system.cpu.dvrDiscoveryCompletions)"
instructions="$(read_stat "$complete_stats" system.cpu.dvrDiscoveredInstructions)"
test -n "$starts" && test "$starts" -gt 0
test -n "$completions" && test "$completions" -gt 0
test -n "$instructions" && test "$instructions" -gt 0

run_case dvr-stage3-timeout 1
timeout_stats="$RESULT_ROOT/dvr-stage3-timeout/stats.txt"
timeouts="$(read_stat "$timeout_stats" system.cpu.dvrDiscoveryTimeouts)"
test -n "$timeouts" && test "$timeouts" -gt 0

printf 'DVR_STAGE3_SMOKE_PASSED starts=%s completions=%s instructions=%s timeouts=%s\n' \
    "$starts" "$completions" "$instructions" "$timeouts"
