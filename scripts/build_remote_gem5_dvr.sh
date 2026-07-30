#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
PYTHON_ROOT="${PYTHON_ROOT:-/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15}"
ZLIB_DEV="${ZLIB_DEV:-/nix/store/h7ik0g1xxayy0z8h27zbvrgmac63irgs-zlib-1.3.2-dev}"
ZLIB_LIB="${ZLIB_LIB:-/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2}"

export PYTHON_CONFIG="$PYTHON_ROOT/bin/python3.11-config"
export CCFLAGS_EXTRA="-I$ZLIB_DEV/include"
export LINKFLAGS_EXTRA="-L$ZLIB_LIB/lib"

cd "$ROOT"
"$PYTHON_ROOT/bin/python3.11" -m SCons build/RISCV/gem5.opt \
    -j"${JOBS:-32}"
