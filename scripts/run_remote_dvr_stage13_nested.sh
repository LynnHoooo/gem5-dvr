#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_nested.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage13-nested}"

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

test -x "$ROOT/build/RISCV/gem5.opt"
if [[ ! -x "$BENCH" ]]; then
    source_file="$ROOT/benchmarks/dvr_nested.c"
    test -f "$source_file"
    compiler=""
    for candidate in riscv64-unknown-linux-gnu-gcc \
                     riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler="$candidate"
            break
        fi
    done
    if [[ -z "$compiler" ]]; then
        printf 'error: no RV64 cross compiler found for %s\n' \
            "$source_file" >&2
        exit 1
    fi
    "$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops \
        -nostdlib -static -march=rv64gc -mabi=lp64d \
        -o "$BENCH" "$source_file"
fi
test -x "$BENCH"
rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

stats="$OUT/stats.txt"
contexts="$(read_stat "$stats" system.cpu.dvrNestedContextsBuilt)"
programs="$(read_stat "$stats" system.cpu.dvrNestedProgramsBuilt)"
vrat="$(read_stat "$stats" system.cpu.dvrNestedVRATAllocations)"
vir="$(read_stat "$stats" system.cpu.dvrNestedVIRExecutions)"
generated="$(read_stat "$stats" system.cpu.dvrNestedHelpersGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrNestedHelpersIssued)"
completed="$(read_stat "$stats" system.cpu.dvrNestedHelpersCompleted)"
nested_flat="$(read_stat "$stats" system.cpu.dvrNestedFlattenedLanes)"
nested_dedup="$(read_stat "$stats" system.cpu.dvrPrefetchesDeduplicated)"
replay_attempts="$(read_stat "$stats" system.cpu.dvrNestedReplayAttempts)"
replay_targets="$(read_stat "$stats" system.cpu.dvrNestedReplayTargetsGenerated)"
replay_fallbacks="$(read_stat "$stats" system.cpu.dvrNestedReplayFallbacks)"
nested_dependent="$(read_stat "$stats" system.cpu.dvrNestedDependentGenerated)"
root_programs="$(read_stat "$stats" system.cpu.dvrVectorProgramsBuilt)"
root_helpers="$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)"

require_nonzero nested_contexts "$contexts"
require_nonzero nested_programs "$programs"
require_nonzero nested_vrat_allocations "$vrat"
require_nonzero nested_vir_executions "$vir"
require_nonzero nested_helpers_generated "$generated"
require_nonzero nested_helpers_issued "$issued"
require_nonzero nested_helpers_completed "$completed"
require_nonzero nested_replay_attempts "$replay_attempts"
require_nonzero nested_replay_targets "$replay_targets"
require_nonzero nested_dependent_generated "$nested_dependent"
if [[ "$issued" -gt "$generated" || "$completed" -ne "$issued" ]]; then
    printf 'error: nested helper lifecycle generated=%s issued=%s completed=%s\n' \
        "$generated" "$issued" "$completed" >&2
    exit 1
fi
if [[ "$replay_targets" -gt "$replay_attempts" ]]; then
    printf 'error: nested replay targets (%s) exceed attempts (%s)\n' \
        "$replay_targets" "$replay_attempts" >&2
    exit 1
fi
if [[ -z "$replay_fallbacks" || "$replay_fallbacks" -ne 0 ]]; then
    printf 'error: expected nested replay fallbacks=0, got %s\n' \
        "${replay_fallbacks:-<missing>}" >&2
    exit 1
fi
require_nonzero all_vector_programs "$root_programs"
require_nonzero all_helpers "$root_helpers"

printf 'DVR_STAGE13_NESTED_PASSED contexts=%s programs=%s vrat=%s vir=%s flattened=%s generated=%s issued=%s completed=%s deduplicated=%s replay_attempts=%s replay_targets=%s replay_fallbacks=%s nested_dependent=%s all_programs=%s all_helpers=%s\n' \
    "$contexts" "$programs" "$vrat" "$vir" "$nested_flat" "$generated" \
    "$issued" "$completed" "$nested_dedup" "$replay_attempts" \
    "$replay_targets" "$replay_fallbacks" "$nested_dependent" \
    "$root_programs" "$root_helpers"
