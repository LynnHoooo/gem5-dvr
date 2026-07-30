#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage10-vrat-vir}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
uops="$(read_stat "$stats" system.cpu.dvrRecordedUops)"
overflows="$(read_stat "$stats" system.cpu.dvrRecorderOverflows)"
programs="$(read_stat "$stats" system.cpu.dvrVectorProgramsBuilt)"
allocations="$(read_stat "$stats" system.cpu.dvrVRATAllocations)"
issues="$(read_stat "$stats" system.cpu.dvrVIRChunkIssues)"
executions="$(read_stat "$stats" system.cpu.dvrVIRChunkExecutions)"
source_completed="$(read_stat "$stats" system.cpu.dvrSourcePrefetchesCompleted)"
replay_supported="$(read_stat "$stats" system.cpu.dvrReplaySupportedUops)"
replay_attempts="$(read_stat "$stats" system.cpu.dvrReplayAttempts)"
replay_targets="$(read_stat "$stats" system.cpu.dvrReplayTargetsGenerated)"
replay_fallbacks="$(read_stat "$stats" system.cpu.dvrReplayFallbacks)"
test -n "$uops" && test "$uops" -gt 0
test -n "$programs" && test "$programs" -gt 0
test -n "$allocations" && test "$allocations" -gt 0
test -n "$issues" && test "$issues" -gt 0
test "$executions" -eq "$issues"
# This stage uses dvr_dependent.riscv, so require real recorded-uop replay in
# addition to structural VRAT/VIR activity.  The accounting equality proves
# that every completed source either produced a replay target or took the
# explicitly counted affine fallback path.
test -n "$source_completed" && test "$source_completed" -gt 0
test -n "$replay_supported" && test "$replay_supported" -gt 0
test -n "$replay_attempts" && test "$replay_attempts" -gt 0
test -n "$replay_targets" && test "$replay_targets" -gt 0
test -n "$replay_fallbacks" && test "$replay_fallbacks" -ge 0
test "$replay_targets" -eq "$replay_attempts"
test $((replay_targets + replay_fallbacks)) -eq "$source_completed"

printf 'DVR_STAGE10_SMOKE_PASSED uops=%s overflows=%s programs=%s vrat_allocations=%s vir_issues=%s vir_executions=%s replay_supported=%s replay_attempts=%s replay_targets=%s replay_fallbacks=%s\n' \
    "$uops" "$overflows" "$programs" "$allocations" "$issues" \
    "$executions" "$replay_supported" "$replay_attempts" \
    "$replay_targets" "$replay_fallbacks"
