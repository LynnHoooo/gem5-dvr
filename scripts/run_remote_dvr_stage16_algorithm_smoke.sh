#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
CXX="${CXX:-c++}"
OUT="${OUT:-${TMPDIR:-/tmp}/dvr_nested_algorithm_smoke}"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
    -I "$ROOT/src" \
    "$ROOT/src/cpu/o3/dvr_nested.cc" \
    "$ROOT/src/cpu/o3/dvr_nested_smoke.cc" \
    -o "$OUT"
"$OUT"
printf 'DVR_STAGE16_ALGORITHM_SMOKE_PASSED\n'
