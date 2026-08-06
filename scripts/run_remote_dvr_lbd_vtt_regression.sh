#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-/home/lynnhoo/dvr-repro/source/gem5-dvr/code/gem5-runahead-dev-pre}"
PROJECT="$(cd -- "$ROOT/../.." && pwd)"
BENCH="$PROJECT/benchmarks/dvr_lbd_vtt.riscv"
SOURCE="$PROJECT/benchmarks/dvr_lbd_vtt.c"
OUT="${OUT:-/home/lynnhoo/dvr-repro/results/dvr-lbd-vtt}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"

compiler=""
# Prefer a compiler available on PATH, but keep the repository's pinned
# RISC-V toolchain as a reproducible fallback for clean servers.
for candidate in riscv64-unknown-linux-gnu-gcc riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc \
    /home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc; do
    if command -v "$candidate" >/dev/null 2>&1; then compiler="$candidate"; break; fi
done
[[ -n "$compiler" ]] || { echo "missing RISC-V compiler" >&2; exit 1; }
"$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops -nostdlib -static \
    -march=rv64gc -mabi=lp64d -o "$BENCH" "$SOURCE"

if [[ -n "${OBJDUMP:-}" ]]; then
    objdump="$OBJDUMP"
else
    compiler_name="${compiler##*/}"
    objdump="$(dirname "$compiler")/${compiler_name%-gcc}-objdump"
fi
[[ -x "$objdump" ]] || { echo "missing RISC-V objdump: $objdump" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
"$objdump" -d "$BENCH" >"$OUT.disasm"
grep -qE '\bslt\b|\bsltu\b' "$OUT.disasm"
# GCC may print the compressed/alias spelling bnez instead of bne.
grep -qE '\bbne(z)?\b|\bblt(u)?\b|\bbge(u)?\b' "$OUT.disasm"

rm -rf "$OUT"; mkdir -p "$OUT"
"$GEM5" --outdir="$OUT" "$CONFIG" --cmd="$BENCH" --dvr --dvr-mode=full \
    --dvr-vector-chunks >"$OUT/stdout.log" 2>&1
stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
matches="$(stat "$OUT/stats.txt" system.cpu.dvrLoopBoundMatches)"
fallbacks="$(stat "$OUT/stats.txt" system.cpu.dvrLoopBoundFallbacks)"
rollbacks="$(stat "$OUT/stats.txt" system.cpu.dvrDiscoveryRollbacks)"
[[ "${matches:-0}" -gt 0 && "${fallbacks:-0}" -gt 0 ]]
echo "DVR_LBD_VTT_PASSED matches=$matches fallbacks=$fallbacks rollbacks=${rollbacks:-0} out=$OUT"
