#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
if [[ ! -d "$BENCH_ROOT" && -d "$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks" ]]; then
    BENCH_ROOT="$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks"
fi
BENCH="${BENCH:-$BENCH_ROOT/dvr_ndm.riscv}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"

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
    source_file="$BENCH_ROOT/dvr_ndm.c"
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

test -x "$GEM5"
test -x "$BENCH"

run_case() {
    local name="$1" threshold="$2" ndm_max="$3"
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$GEM5" --outdir="$out" \
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
ir="$(read_stat "$normal" system.cpu.dvrNDMIRCaptures)"
ilr="$(read_stat "$normal" system.cpu.dvrNDMILRCaptures)"
lcr="$(read_stat "$normal" system.cpu.dvrNDMLCRCaptures)"
outer_invocations="$(read_stat "$normal" system.cpu.dvrNDMOuterInvocations)"
helper_fetch="$(read_stat "$normal" system.cpu.dvrHelperFetchCycles)"
require_nonzero ndm_attempts "$attempts"
require_nonzero ndm_outer_found "$outer"
require_nonzero ordinary_helpers "$helpers"
require_nonzero ndm_ir_captures "$ir"
require_nonzero ndm_ilr_captures "$ilr"
require_nonzero ndm_lcr_captures "$lcr"
require_nonzero ndm_outer_invocations "$outer_invocations"
require_nonzero helper_fetch_cycles "$helper_fetch"
if [[ -z "$fallbacks" || "$fallbacks" -ne 0 ]]; then
    printf 'error: expected successful NDM fallbacks=0, got %s\n' \
        "${fallbacks:-<missing>}" >&2
    exit 1
fi
if (( outer_invocations < 2 )); then
    printf 'error: expected at least two NDM outer invocations, got %s\n' \
        "$outer_invocations" >&2
    exit 1
fi

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

printf 'DVR_STAGE14_NDM_CONTROL_PASSED attempts=%s outer=%s fallbacks=%s helpers=%s ir=%s ilr=%s lcr=%s outer_invocations=%s helper_fetch=%s disabled_attempts=%s timeouts=%s timeout_fallbacks=%s\n' \
    "$attempts" "$outer" "$fallbacks" "$helpers" "$ir" "$ilr" \
    "$lcr" "$outer_invocations" "$helper_fetch" \
    "$disabled_attempts" "$timeouts" "$timeout_fallbacks"
