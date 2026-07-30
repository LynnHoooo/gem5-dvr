#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results}"
VENV="${VENV:-$HOME/dvr-repro/venv311}"

python_real="$(readlink -f "$VENV/bin/python3")"
python_root="$(dirname "$(dirname "$python_real")")"
export LD_LIBRARY_PATH="$python_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cd "$ROOT"

gem5="$ROOT/build/RISCV/gem5.opt"
hello="$ROOT/tests/test-progs/hello/bin/riscv/linux/hello"

test -x "$gem5"
test -x "$hello"

mkdir -p "$RESULT_ROOT/smoke-baseline" "$RESULT_ROOT/smoke-pre"

"$gem5" -d "$RESULT_ROOT/smoke-baseline" configs/example/se.py \
    --cpu-type=DerivO3CPU --caches --cmd="$hello" \
    >"$RESULT_ROOT/smoke-baseline/run.log" 2>&1

"$gem5" -d "$RESULT_ROOT/smoke-pre" configs/example/se.py \
    --cpu-type=DerivO3CPU --caches --cmd="$hello" \
    --param 'system.cpu[0].enablePRE=True' \
    >"$RESULT_ROOT/smoke-pre/run.log" 2>&1

grep -q '^enablePRE=true$' "$RESULT_ROOT/smoke-pre/config.ini"
grep -q 'Hello world!' "$RESULT_ROOT/smoke-baseline/run.log"
grep -q 'Hello world!' "$RESULT_ROOT/smoke-pre/run.log"

echo "PRE_SMOKE_PASSED"
