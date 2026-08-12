#!/usr/bin/env bash
set -euo pipefail

# VR 复现 Stage 0：验证 PRE 是 VR 的可用基座。
# 在 vr_indirect 微基准上跑 OoO 基线与 PRE 两组，确认：
#   1. 两组都正常退出（微基准本身无 RVV/浮点依赖）；
#   2. PRE 组的 enablePRE 确实生效；
#   3. 提取 numCycles / L1D 缺失，为 Stage 1 之后的 OoO/PRE/VR 三方对比留底。
#
# 用法（服务器上）：
#   ROOT=/path/to/gem5-runahead-dev-pre \
#   RESULT_ROOT=/path/to/results/vr-stage0 \
#   bash scripts/run_remote_vr_stage0_prebase.sh

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results/vr-stage0-prebase}"
PYTHON_ROOT="${PYTHON_ROOT:-/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15}"

export LD_LIBRARY_PATH="$PYTHON_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}

gem5="$ROOT/build/RISCV/gem5.opt"
source_file="$ROOT/benchmarks/vr_indirect.c"
binary="$ROOT/benchmarks/vr_indirect.riscv"

test -x "$gem5"
test -f "$source_file"

# 服务器上若无现成二进制则就地交叉编译（与 DVR Stage 13 脚本同款命令）。
if [[ ! -x "$binary" ]]; then
    compiler=""
    for candidate in riscv64-unknown-linux-gnu-gcc \
                     riscv64-linux-gnu-gcc riscv64-unknown-elf-gcc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            compiler="$candidate"
            break
        fi
    done
    if [[ -z "$compiler" ]]; then
        printf 'error: no RV64 cross compiler found for %s\n' \
            "$source_file" >&2
        exit 1
    fi
    "$compiler" -O2 -fno-tree-vectorize -fno-unroll-loops \
        -nostdlib -static -march=rv64gc -mabi=lp64d \
        -o "$binary" "$source_file"
fi
test -x "$binary"

mkdir -p "$RESULT_ROOT/baseline" "$RESULT_ROOT/pre"

"$gem5" -d "$RESULT_ROOT/baseline" "$ROOT/configs/example/se.py" \
    --cpu-type=DerivO3CPU --caches --cmd="$binary" \
    >"$RESULT_ROOT/baseline/run.log" 2>&1

"$gem5" -d "$RESULT_ROOT/pre" "$ROOT/configs/example/se.py" \
    --cpu-type=DerivO3CPU --caches --cmd="$binary" \
    --param 'system.cpu[0].enablePRE=True' \
    >"$RESULT_ROOT/pre/run.log" 2>&1

grep -q '^enablePRE=true$' "$RESULT_ROOT/pre/config.ini"
grep -q 'exiting with last active thread context' \
    "$RESULT_ROOT/baseline/run.log"
grep -q 'exiting with last active thread context' \
    "$RESULT_ROOT/pre/run.log"

base_cycles="$(read_stat "$RESULT_ROOT/baseline/stats.txt" system.cpu.numCycles)"
pre_cycles="$(read_stat "$RESULT_ROOT/pre/stats.txt" system.cpu.numCycles)"
base_misses="$(read_stat "$RESULT_ROOT/baseline/stats.txt" system.cpu.dcache.ReadReq.misses::cpu.data)"
pre_misses="$(read_stat "$RESULT_ROOT/pre/stats.txt" system.cpu.dcache.ReadReq.misses::cpu.data)"

test -n "$base_cycles"
test -n "$pre_cycles"

printf 'VR_STAGE0_PREBASE_PASSED baseline_cycles=%s pre_cycles=%s baseline_l1d_misses=%s pre_l1d_misses=%s\n' \
    "$base_cycles" "$pre_cycles" "${base_misses:-0}" "${pre_misses:-0}"
