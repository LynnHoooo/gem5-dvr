#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
VENV="${VENV:-$HOME/dvr-repro/venv}"
PYTHON_ROOT="${PYTHON_ROOT:-}"
ZLIB_DEV="${ZLIB_DEV:-/nix/store/h7ik0g1xxayy0z8h27zbvrgmac63irgs-zlib-1.3.2-dev}"
ZLIB_LIB="${ZLIB_LIB:-/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2}"

if [[ -n "$PYTHON_ROOT" ]]; then
    PYTHON_BIN="$PYTHON_ROOT/bin/python3"
elif [[ -x "$VENV/bin/python" && -x "$VENV/bin/scons" ]]; then
    PYTHON_BIN="$VENV/bin/python"
else
    echo "error: set PYTHON_ROOT or VENV with python and scons" >&2
    exit 2
fi

test -x "$PYTHON_BIN"
PYTHON_CONFIG="${PYTHON_CONFIG:-$("$PYTHON_BIN" -c \
    'import sys; print(sys.base_prefix)' )/bin/python3-config}"
test -x "$PYTHON_CONFIG"
PYTHON_VERSION="$("$PYTHON_BIN" -c \
    'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
case "$PYTHON_VERSION" in
    3.8|3.9|3.10|3.11|3.12) ;;
    *)
        echo "error: gem5 requires Python 3.8-3.12; found $PYTHON_VERSION" >&2
        exit 2
        ;;
esac
test -d "$ZLIB_DEV/include"
test -d "$ZLIB_LIB/lib"

export PYTHON_CONFIG
export CCFLAGS_EXTRA="-I$ZLIB_DEV/include"
export LINKFLAGS_EXTRA="-L$ZLIB_LIB/lib"

cd "$ROOT"
"$PYTHON_BIN" -m SCons build/RISCV/gem5.opt \
    -j"${JOBS:-32}"
