#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_ndm.riscv}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}
require_nonzero() {
    local label="$1" value="$2"
    if [[ -z "$value" || "$value" -le 0 ]]; then
        printf 'error: expected %s > 0, got %s\n' \
            "$label" "${value:-<missing>}" >&2
        exit 1
    fi
}

if [[ ! -x "$BENCH" ]]; then
    source_file="$ROOT/benchmarks/dvr_ndm.c"
    test -f "$source_file"
    compiler=""
    for candidate in riscv64-unknown-linux-gnu-gcc \
                     riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler="$candidate"
            break
        fi
    done
    test -n "$compiler"
    "$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops \
        -nostdlib -static -march=rv64gc -mabi=lp64d \
        -o "$BENCH" "$source_file"
fi

test -x "$ROOT/build/RISCV/gem5.opt"
test -x "$BENCH"

run_case() {
    local name="$1" threshold="$2" ndm_max="$3"
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr \
        --dvr-ndm-threshold="$threshold" \
        --dvr-ndm-max-insts="$ndm_max"
}

run_case dvr-stage14-ndm-control 64 512
normal="$RESULT_ROOT/dvr-stage14-ndm-control/stats.txt"
attempts="$(read_stat "$normal" system.cpu.dvrNDMAttempts)"
outer="$(read_stat "$normal" system.cpu.dvrNDMOuterFound)"
fallbacks="$(read_stat "$normal" system.cpu.dvrNDMFallbacks)"
helpers="$(read_stat "$normal" system.cpu.dvrPrefetchesGenerated)"
require_nonzero ndm_attempts "$attempts"
require_nonzero ndm_outer_found "$outer"
require_nonzero ndm_fallbacks "$fallbacks"
require_nonzero ordinary_helpers "$helpers"

run_case dvr-stage14-ndm-disabled 1 512
disabled="$RESULT_ROOT/dvr-stage14-ndm-disabled/stats.txt"
disabled_attempts="$(read_stat "$disabled" system.cpu.dvrNDMAttempts)"
if [[ -z "$disabled_attempts" || "$disabled_attempts" -ne 0 ]]; then
    printf 'error: expected threshold=1 attempts=0, got %s\n' \
        "${disabled_attempts:-<missing>}" >&2
    exit 1
fi

run_case dvr-stage14-ndm-timeout 64 1
timeout="$RESULT_ROOT/dvr-stage14-ndm-timeout/stats.txt"
timeouts="$(read_stat "$timeout" system.cpu.dvrNDMTimeouts)"
timeout_fallbacks="$(read_stat "$timeout" system.cpu.dvrNDMFallbacks)"
require_nonzero ndm_timeouts "$timeouts"
require_nonzero ndm_timeout_fallbacks "$timeout_fallbacks"

printf 'DVR_STAGE14_NDM_CONTROL_PASSED attempts=%s outer=%s fallbacks=%s helpers=%s disabled_attempts=%s timeouts=%s timeout_fallbacks=%s\n' \
    "$attempts" "$outer" "$fallbacks" "$helpers" "$disabled_attempts" \
    "$timeouts" "$timeout_fallbacks"
