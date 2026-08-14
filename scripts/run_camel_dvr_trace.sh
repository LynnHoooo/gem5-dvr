#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ROOT:-$SCRIPT_DIR/../code/gem5-runahead-dev-pre}"
CAMEL_SRC="${CAMEL_SRC:-/home/lynnhoo/gem-test/gem5-leap/leap-bench/hpc/camel/camel.c}"
CAMEL_CC="${CAMEL_CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"
M5_ROOT="${M5_ROOT:-$ROOT/util/m5}"
M5_LIB="${M5_LIB:-$M5_ROOT/build/riscv/out/libm5.a}"
MAX_KEY="${MAX_KEY:-65536}"
OUT="${OUT:-$ROOT/../../../../results/camel-dvr-trace-max${MAX_KEY}}"
DVR_MODE="${DVR_MODE:-nested}"
BENCH="$OUT/camel.riscv"

test -f "$CAMEL_SRC"
test -x "$CAMEL_CC"
test -s "$M5_LIB"
mkdir -p "$OUT"

"$CAMEL_CC" -O2 -fno-tree-vectorize -static -DMAX_KEY="$MAX_KEY" \
    -DENABLE_GEM5_STATS -I"$ROOT/include" \
    -o "$BENCH" "$CAMEL_SRC" "$M5_LIB"
file "$BENCH" | grep -q 'RISC-V'

DVR_TRACE_DIR="$OUT" DVR_PC_SUMMARY_DIR="$OUT" \
    "$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" \
    --dvr --dvr-mode="$DVR_MODE" --dvr-vector-chunks \
    --dvr-ndm-max-insts="${NDM_MAX_INSTS:-2048}" \
    >"$OUT/stdout.log" 2>&1

awk -v out="$OUT" -v max_key="$MAX_KEY" '
BEGIN { printf "{\n  \"workload\": \"camel\",\n  \"max_key\": %d,\n", max_key + 0 }
END {
  printf "  \"files\": {\n"
  printf "    \"workload_csv\": \"%s/workload.csv\",\n", out
  printf "    \"dependency_csv\": \"%s/dependency_chain.csv\",\n", out
  printf "    \"vectorization_csv\": \"%s/vectorization.csv\",\n", out
  printf "    \"events_jsonl\": \"%s/events.jsonl\"\n", out
  printf "  }\n}\n"
}' < /dev/null >"$OUT/summary.json"

for file in workload.csv dependency_chain.csv vectorization.csv events.jsonl summary.json stats.txt; do
    test -s "$OUT/$file"
done

python3 "$SCRIPT_DIR/summarize_camel_dvr_trace.py" "$OUT"
test -s "$OUT/dependency_summary.json"
test -s "$OUT/dependency_summary.csv"
test -s "$OUT/pc_pipeline_summary.csv"

printf 'CAMEL_DVR_TRACE_PASSED out=%s max_key=%s\n' "$OUT" "$MAX_KEY"
