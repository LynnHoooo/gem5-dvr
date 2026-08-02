#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
if [[ ! -d "$BENCH_ROOT" && -d "$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks" ]]; then
    BENCH_ROOT="$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks"
fi
BENCH="${BENCH:-$BENCH_ROOT/dvr_divergent.riscv}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

require_nonzero() {
    local label="$1"
    local value="$2"
    if [[ -z "$value" || "$value" -le 0 ]]; then
        printf 'error: expected %s > 0, got %s\n' \
            "$label" "${value:-<missing>}" >&2
        return 1
    fi
}

require_equal() {
    local label="$1"
    local actual="$2"
    local expected="$3"
    if [[ -z "$actual" || "$actual" -ne "$expected" ]]; then
        printf 'error: expected %s=%s, got %s\n' \
            "$label" "$expected" "${actual:-<missing>}" >&2
        return 1
    fi
}

run_case() {
    local name="$1"
    local budget="$2"
    local out="$RESULT_ROOT/$name"
    rm -rf "$out"
    mkdir -p "$out"
    "$ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr \
        --dvr-helper-max-uops="$budget"
}

test -x "$ROOT/build/RISCV/gem5.opt"
test -x "$BENCH"

run_case dvr-stage11-control 200
normal="$RESULT_ROOT/dvr-stage11-control/stats.txt"
starts="$(read_stat "$normal" system.cpu.dvrDiscoveryStarts)"
completions="$(read_stat "$normal" system.cpu.dvrDiscoveryCompletions)"
abandons="$(read_stat "$normal" system.cpu.dvrDiscoveryAbandons)"
programs="$(read_stat "$normal" system.cpu.dvrVectorProgramsBuilt)"
recorder_overflows="$(read_stat "$normal" system.cpu.dvrRecorderOverflows)"
branches="$(read_stat "$normal" system.cpu.dvrDivergentBranches)"
reconvergences="$(read_stat "$normal" system.cpu.dvrReconvergences)"
predicate_abandons="$(read_stat "$normal" system.cpu.dvrPredicateGenerationAbandons)"
timeouts="$(read_stat "$normal" system.cpu.dvrHelperTimeouts)"
overflows="$(read_stat "$normal" system.cpu.dvrReconvergenceStackOverflows)"
unsupported="$(read_stat "$normal" system.cpu.dvrVIRUnsupportedControlFlow)"
relations="$(read_stat "$normal" system.cpu.dvrAddressRelationsTrained)"
paths="$(read_stat "$normal" system.cpu.dvrDistinctPredicatePaths)"
dependent="$(read_stat "$normal" system.cpu.dvrDependentPrefetchesGenerated)"
require_nonzero discovery_starts "$starts"
require_nonzero discovery_completions "$completions"
require_nonzero vector_programs "$programs"
require_nonzero divergent_branches "$branches"
if [[ -z "$reconvergences" || -z "$predicate_abandons" ||
      -z "$unsupported" ||
      $((reconvergences + predicate_abandons + unsupported)) -lt "$branches" ]]; then
    printf 'error: actual predicate generations did not terminate: '
    printf 'divergent=%s reconverged=%s abandoned=%s unsupported=%s\n' \
        "$branches" "${reconvergences:-<missing>}" \
        "${predicate_abandons:-<missing>}" "${unsupported:-<missing>}" >&2
    exit 1
fi
require_equal helper_timeouts "$timeouts" 0
require_equal reconvergence_stack_overflows "$overflows" 0
require_equal recorder_overflows "$recorder_overflows" 0
if [[ -z "$relations" || "$relations" -lt 2 ]]; then
    printf 'error: expected at least two trained FLR relations, got %s '\
'(starts=%s completions=%s abandons=%s)\n' \
        "${relations:-<missing>}" "$starts" "$completions" "$abandons" >&2
    exit 1
fi
if [[ -z "$paths" || ( "$paths" -lt 2 && "$unsupported" -eq 0 ) ]]; then
    printf 'error: expected both value-predicate paths, got %s '\
'(relations=%s dependent=%s)\n' \
        "${paths:-<missing>}" "$relations" "$dependent" >&2
    exit 1
fi
require_nonzero dependent_prefetches "$dependent"

run_case dvr-stage11-timeout 1
limited="$RESULT_ROOT/dvr-stage11-timeout/stats.txt"
limited_timeouts="$(read_stat "$limited" system.cpu.dvrHelperTimeouts)"
limited_generated="$(read_stat "$limited" system.cpu.dvrPrefetchesGenerated)"
require_nonzero forced_helper_timeouts "$limited_timeouts"
require_equal forced_prefetches_generated "$limited_generated" 0

printf 'DVR_STAGE11_CONTROL_PASSED starts=%s completions=%s abandons=%s programs=%s divergent=%s reconvergences=%s predicate_abandons=%s unsupported_control_flow=%s relations=%s selected_paths=%s dependent=%s timeouts=%s recorder_overflows=%s stack_overflows=%s forced_timeouts=%s forced_generated=%s\n' \
    "$starts" "$completions" "$abandons" "$programs" "$branches" \
    "$reconvergences" "$predicate_abandons" "$unsupported" "$relations" "$paths" "$dependent" "$timeouts" \
    "$recorder_overflows" "$overflows" \
    "$limited_timeouts" "$limited_generated"
