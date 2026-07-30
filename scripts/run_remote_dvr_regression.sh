#!/usr/bin/env bash
set -euo pipefail

# Run from the remote Nix development shell:
#   QUICK=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
#   ~/dvr-repro/scripts/run_remote_dvr_regression.sh
#
# QUICK=1 is a structural/short functional regression.  It skips the build,
# the duplicate Stage 5/6 runs, cache-traffic-heavy Stages 7/8, the two-run
# Stage 9 comparison, and the two-run Stage 11 control-flow test.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"
REGRESSION_LOG_ROOT="${REGRESSION_LOG_ROOT:-$RESULT_ROOT/dvr-regression-logs}"
QUICK="${QUICK:-0}"
SKIP_BUILD="${SKIP_BUILD:-0}"

case "$QUICK" in
    0|1) ;;
    *) printf 'error: QUICK must be 0 or 1 (got %s)\n' "$QUICK" >&2; exit 2 ;;
esac
case "$SKIP_BUILD" in
    0|1) ;;
    *) printf 'error: SKIP_BUILD must be 0 or 1 (got %s)\n' "$SKIP_BUILD" >&2; exit 2 ;;
esac

test -d "$ROOT"
mkdir -p "$REGRESSION_LOG_ROOT"
run_id="$(date -u +%Y%m%dT%H%M%SZ)"
summary="$REGRESSION_LOG_ROOT/$run_id.summary"
: >"$summary"

run_step() {
    local name="$1"
    local script="$2"
    local log="$REGRESSION_LOG_ROOT/$run_id.$name.log"
    printf '[RUN ] %s\n' "$name" | tee -a "$summary"
    if ROOT="$ROOT" bash "$script" 2>&1 | tee "$log"; then
        printf '[PASS] %s log=%s\n' "$name" "$log" | tee -a "$summary"
    else
        local status="${PIPESTATUS[0]}"
        printf '[FAIL] %s status=%s log=%s\n' "$name" "$status" "$log" |
            tee -a "$summary"
        return "$status"
    fi
}

skip_step() {
    printf '[SKIP] %s (%s)\n' "$1" "$2" | tee -a "$summary"
}

if [[ "$SKIP_BUILD" == 1 || "$QUICK" == 1 ]]; then
    skip_step build "SKIP_BUILD=$SKIP_BUILD QUICK=$QUICK"
else
    run_step build "$SCRIPT_DIR/build_remote_gem5_dvr.sh"
fi

test -x "$ROOT/build/RISCV/gem5.opt"
run_step stage1-rpt "$SCRIPT_DIR/run_remote_dvr_stage1_smoke.sh"
run_step stage2-table1 "$SCRIPT_DIR/run_remote_table1_smoke.sh"
run_step stage3-discovery "$SCRIPT_DIR/run_remote_dvr_stage3_smoke.sh"
run_step stage4-vtt-flr "$SCRIPT_DIR/run_remote_dvr_stage4_smoke.sh"

if [[ "$QUICK" == 1 ]]; then
    skip_step stage5-loop-bound "covered by the same dependent benchmark; full only"
    skip_step stage6-lane-count "covered by the same dependent benchmark; full only"
    skip_step stage7-cache-injection "cache-traffic-heavy; full only"
    skip_step stage8-dependent-prefetch "cache-traffic-heavy; full only"
    skip_step stage9-compare "two full simulations; full only"
else
    run_step stage5-loop-bound "$SCRIPT_DIR/run_remote_dvr_stage5_smoke.sh"
    run_step stage6-lane-count "$SCRIPT_DIR/run_remote_dvr_stage6_smoke.sh"
    run_step stage7-cache-injection "$SCRIPT_DIR/run_remote_dvr_stage7_smoke.sh"
    run_step stage8-dependent-prefetch "$SCRIPT_DIR/run_remote_dvr_stage8_smoke.sh"
    run_step stage9-compare "$SCRIPT_DIR/run_remote_dvr_stage9_compare.sh"
fi

run_step stage10-vrat-vir "$SCRIPT_DIR/run_remote_dvr_stage10_smoke.sh"
if [[ "$QUICK" == 1 ]]; then
    skip_step stage11-control-flow "normal plus forced-timeout simulations; full only"
else
    run_step stage11-control-flow "$SCRIPT_DIR/run_remote_dvr_stage11_control_flow.sh"
fi

printf 'DVR_REGRESSION_PASSED quick=%s summary=%s\n' "$QUICK" "$summary" |
    tee -a "$summary"
