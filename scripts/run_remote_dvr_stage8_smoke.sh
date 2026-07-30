#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage8-dependent-prefetch}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

stats="$OUT/stats.txt"
relations="$(read_stat "$stats" system.cpu.dvrAddressRelationsTrained)"
generated="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesGenerated)"
issued="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.dvrDependentPrefetchesCompleted)"
source_completed="$(read_stat "$stats" system.cpu.dvrSourcePrefetchesCompleted)"
replay_supported="$(read_stat "$stats" system.cpu.dvrReplaySupportedUops)"
replay_attempts="$(read_stat "$stats" system.cpu.dvrReplayAttempts)"
replay_targets="$(read_stat "$stats" system.cpu.dvrReplayTargetsGenerated)"
replay_fallbacks="$(read_stat "$stats" system.cpu.dvrReplayFallbacks)"
total_issued="$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
total_completed="$(read_stat "$stats" system.cpu.dvrPrefetchesCompleted)"
issued_bytes="$(read_stat "$stats" system.cpu.dvrQualityIssuedBytes)"
completed_bytes="$(read_stat "$stats" system.cpu.dvrQualityCompletedBytes)"
test -n "$relations" && test "$relations" -gt 0
test -n "$generated" && test "$generated" -gt 0
test -n "$issued" && test "$issued" -gt 0
test -n "$completed" && test "$completed" -gt 0
test "$completed" -le "$issued"
# dvr_dependent.riscv is deliberately constructed so that the recorded
# trigger-to-FLR chain is executable by the replay evaluator.  Merely seeing
# affine dependent prefetches is therefore not sufficient for this stage.
test -n "$source_completed" && test "$source_completed" -gt 0
test -n "$replay_supported" && test "$replay_supported" -gt 0
test -n "$replay_attempts" && test "$replay_attempts" -gt 0
test -n "$replay_targets" && test "$replay_targets" -gt 0
test -n "$replay_fallbacks" && test "$replay_fallbacks" -ge 0
test "$replay_targets" -eq "$replay_attempts"
test $((replay_targets + replay_fallbacks)) -eq "$source_completed"
test -n "$issued_bytes" && test "$issued_bytes" -eq $((total_issued * 8))
test -n "$completed_bytes" &&
    test "$completed_bytes" -eq $((total_completed * 8))

printf 'DVR_STAGE8_SMOKE_PASSED relations=%s generated=%s issued=%s completed=%s replay_supported=%s replay_attempts=%s replay_targets=%s replay_fallbacks=%s issued_bytes=%s completed_bytes=%s\n' \
    "$relations" "$generated" "$issued" "$completed" \
    "$replay_supported" "$replay_attempts" "$replay_targets" \
    "$replay_fallbacks" "$issued_bytes" "$completed_bytes"
