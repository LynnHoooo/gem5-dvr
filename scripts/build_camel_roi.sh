#!/usr/bin/env bash
set -euo pipefail

# Build the Camel variant that brackets a repeated kernel with gem5 ROI
# annotations.  The original Camel source remains unchanged.
ROOT="${ROOT:-/home/lynnhoo/dvr-repro/source/gem5-dvr/code/gem5-runahead-dev-pre}"
SRC="${SRC:-/home/lynnhoo/gem-test/gem5-leap/leap-bench/hpc/camel/camel_roi.c}"
CC="${CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"
M5_LIB="${M5_LIB:-$ROOT/util/m5/build/riscv/out/libm5.a}"
OUT="${OUT:-/home/lynnhoo/dvr-repro/results/camel-roi-build}"
MAX_KEY="${MAX_KEY:-65536}"
ROI_REPEATS="${ROI_REPEATS:-73}"

[[ -f "$SRC" ]] || { echo "missing Camel ROI source: $SRC" >&2; exit 1; }
[[ -s "$M5_LIB" ]] || { echo "missing m5 library: $M5_LIB" >&2; exit 1; }
mkdir -p "$OUT"

"$CC" -O2 -fno-tree-vectorize -static -march=rv64gc -mcmodel=medany \
    -DMAX_KEY="$MAX_KEY" -DROI_REPEATS="$ROI_REPEATS" \
    -DENABLE_GEM5_STATS -I"$ROOT/include" \
    -o "$OUT/camel_roi.riscv" "$SRC" "$M5_LIB" -lm

echo "built $OUT/camel_roi.riscv (MAX_KEY=$MAX_KEY ROI_REPEATS=$ROI_REPEATS)"
