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
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"

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
    "$GEM5" --outdir="$out" \
        "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr \
        --dvr-helper-max-uops="$budget"
}

test -x "$GEM5"
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
normal_terminated="$(read_stat "$normal" system.cpu.dvrVIRNormalTerminatedLanes)"
early_exits="$(read_stat "$normal" system.cpu.dvrVIREarlyExitLanes)"
external_lanes="$(read_stat "$normal" system.cpu.dvrVIRExternalPathLanes)"
semantic_lanes="$(read_stat "$normal" system.cpu.dvrVIRUnsupportedSemanticLanes)"
predicate_abandons="$(read_stat "$normal" system.cpu.dvrPredicateGenerationAbandons)"
timeouts="$(read_stat "$normal" system.cpu.dvrHelperTimeouts)"
overflows="$(read_stat "$normal" system.cpu.dvrReconvergenceStackOverflows)"
unsupported="$(read_stat "$normal" system.cpu.dvrVIRUnsupportedControlFlow)"
relations="$(read_stat "$normal" system.cpu.dvrAddressRelationsTrained)"
paths="$(read_stat "$normal" system.cpu.dvrDistinctPredicatePaths)"
dependent="$(read_stat "$normal" system.cpu.dvrDependentPrefetchesGenerated)"
control_fallback="$(read_stat "$normal" system.cpu.dvrControlFallbackSourceLaunches)"
source_value_execs="$(read_stat "$normal" system.cpu.dvrVIRSourceValueExecutions)"
source_value_external="$(read_stat "$normal" system.cpu.dvrVIRSourceValueExternalLanes)"
continuation_contexts="$(read_stat "$normal" system.cpu.dvrVIRContinuationContexts)"
continuation_resumes="$(read_stat "$normal" system.cpu.dvrVIRContinuationResumes)"
require_nonzero discovery_starts "$starts"
require_nonzero discovery_completions "$completions"
require_nonzero vector_programs "$programs"
require_nonzero source_value_vir_executions "$source_value_execs"
require_nonzero vir_continuation_contexts "$continuation_contexts"
require_nonzero vir_continuation_resumes "$continuation_resumes"
if [[ -z "$branches" || -z "$normal_terminated" || -z "$early_exits" ||
      -z "$external_lanes" || -z "$semantic_lanes" ]]; then
    printf 'error: VIR termination counters are missing\n' >&2
    exit 1
fi
if (( branches == 0 && normal_terminated + early_exits + external_lanes == 0 )); then
    printf 'error: no divergence and no lane termination observed\n' >&2
    exit 1
fi
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
if (( branches > 0 || unsupported == 0 )); then
    require_nonzero dependent_prefetches "$dependent"
fi

run_case dvr-stage11-timeout 1
limited="$RESULT_ROOT/dvr-stage11-timeout/stats.txt"
limited_timeouts="$(read_stat "$limited" system.cpu.dvrHelperTimeouts)"
limited_generated="$(read_stat "$limited" system.cpu.dvrPrefetchesGenerated)"
require_nonzero forced_helper_timeouts "$limited_timeouts"
require_equal forced_prefetches_generated "$limited_generated" 0

printf 'DVR_STAGE11_CONTROL_PASSED starts=%s completions=%s abandons=%s programs=%s divergent=%s reconvergences=%s predicate_abandons=%s normal_terminated=%s early_exits=%s external_lanes=%s semantic_lanes=%s unsupported_control_flow=%s control_fallback_source_launches=%s vir_continuation_contexts=%s vir_continuation_resumes=%s source_value_vir_executions=%s source_value_external_lanes=%s relations=%s selected_paths=%s dependent=%s timeouts=%s recorder_overflows=%s stack_overflows=%s forced_timeouts=%s forced_generated=%s\n' \
    "$starts" "$completions" "$abandons" "$programs" "$branches" \
    "$reconvergences" "$predicate_abandons" "$normal_terminated" "$early_exits" "$external_lanes" "$semantic_lanes" "$unsupported" "$control_fallback" "$continuation_contexts" "$continuation_resumes" "$source_value_execs" "$source_value_external" "$relations" "$paths" "$dependent" "$timeouts" \
    "$recorder_overflows" "$overflows" \
    "$limited_timeouts" "$limited_generated"
