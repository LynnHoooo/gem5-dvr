#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
OUT="${OUT:-$HOME/dvr-repro/results/dvr-stage15-resource}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_dependent.riscv}"
read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
test -x "$ROOT/build/RISCV/gem5.opt"
test -x "$BENCH"
rm -rf "$OUT"; mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
  "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr
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
for pair in cycles:$cycles issued:$issued conflicts:$conflicts issue_conflicts:$issue_conflicts alu_conflicts:$alu_conflicts lsu_conflicts:$lsu_conflicts main_issue:$main_issue main_suppress:$main_suppress dvr_issued:$dvr_issued; do
  test -n "${pair#*:}"
done
if (( cycles <= 0 || issued <= 0 || dvr_issued <= 0 )); then
  echo "error: resource counters did not observe a running helper" >&2; exit 1
fi
if (( issued > cycles )); then
  echo "error: helper issue cycles exceed CPU cycles" >&2; exit 1
fi
printf 'DVR_STAGE15_RESOURCE_PASSED cycles=%s helper_issue_cycles=%s conflicts=%s issue_conflicts=%s alu_conflicts=%s lsu_conflicts=%s main_issue=%s main_thread_suppressed=%s dvr_issued=%s\n' \
  "$cycles" "$issued" "$conflicts" "$issue_conflicts" "$alu_conflicts" \
  "$lsu_conflicts" "$main_issue" "$main_suppress" "$dvr_issued"
