#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage15-resource}"
BENCH_ROOT="${BENCH_ROOT:-$ROOT/benchmarks}"
if [[ ! -d "$BENCH_ROOT" && -d "$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks" ]]; then
    BENCH_ROOT="$REPO_ROOT/../gem5-runahead-dev-pre/benchmarks"
fi
BENCH="${BENCH:-$BENCH_ROOT/dvr_dependent.riscv}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
test -x "$GEM5"
test -x "$BENCH"
rm -rf "$OUT"; mkdir -p "$OUT"
"$GEM5" --outdir="$OUT" \
  "$CONFIG" --cmd="$BENCH" --dvr
stats="$OUT/stats.txt"
cycles="$(read_stat "$stats" system.cpu.numCycles)"
issued="$(read_stat "$stats" system.cpu.dvrHelperIssueCycles)"
conflicts="$(read_stat "$stats" system.cpu.dvrResourceConflicts)"
issue_conflicts="$(read_stat "$stats" system.cpu.dvrIssueBudgetConflicts)"
alu_conflicts="$(read_stat "$stats" system.cpu.dvrALUBudgetConflicts)"
lsu_conflicts="$(read_stat "$stats" system.cpu.dvrLSUBudgetConflicts)"
main_issue="$(read_stat "$stats" system.cpu.dvrMainIssueSlotsUsed)"
main_suppress="$(read_stat "$stats" system.cpu.dvrPrefetchesSuppressedMainThread)"
dvr_issued="$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
helper_fetch="$(read_stat "$stats" system.cpu.dvrHelperFetchCycles)"
helper_decode="$(read_stat "$stats" system.cpu.dvrHelperDecodeCycles)"
helper_compute="$(read_stat "$stats" system.cpu.dvrHelperComputeCycles)"
helper_alu_ops="$(read_stat "$stats" system.cpu.dvrHelperALUOps)"
helper_shift_ops="$(read_stat "$stats" system.cpu.dvrHelperShiftOps)"
helper_lsu_ops="$(read_stat "$stats" system.cpu.dvrHelperLSUOps)"
for pair in cycles:$cycles issued:$issued helper_fetch:$helper_fetch helper_decode:$helper_decode helper_compute:$helper_compute helper_alu_ops:$helper_alu_ops helper_shift_ops:$helper_shift_ops helper_lsu_ops:$helper_lsu_ops conflicts:$conflicts issue_conflicts:$issue_conflicts alu_conflicts:$alu_conflicts lsu_conflicts:$lsu_conflicts main_issue:$main_issue main_suppress:$main_suppress dvr_issued:$dvr_issued; do
  test -n "${pair#*:}"
done
if (( cycles <= 0 || issued <= 0 || helper_fetch <= 0 || helper_decode <= 0 ||
      helper_compute <= 0 || helper_alu_ops <= 0 || helper_shift_ops <= 0 ||
      helper_lsu_ops <= 0 || dvr_issued <= 0 )); then
  echo "error: resource counters did not observe a running helper" >&2; exit 1
fi
if (( issued > cycles )); then
  echo "error: helper issue cycles exceed CPU cycles" >&2; exit 1
fi
printf 'DVR_STAGE15_RESOURCE_PASSED cycles=%s helper_fetch=%s helper_decode=%s helper_compute=%s alu_ops=%s shift_ops=%s lsu_ops=%s helper_issue_cycles=%s conflicts=%s issue_conflicts=%s alu_conflicts=%s lsu_conflicts=%s main_issue=%s main_thread_suppressed=%s dvr_issued=%s\n' \
  "$cycles" "$helper_fetch" "$helper_decode" "$helper_compute" "$helper_alu_ops" "$helper_shift_ops" "$helper_lsu_ops" "$issued" "$conflicts" "$issue_conflicts" "$alu_conflicts" "$lsu_conflicts" "$main_issue" "$main_suppress" "$dvr_issued"
