#!/usr/bin/env bash
set -euo pipefail

# VR 复现 Stage 1：机制冒烟。
# 在 vr_indirect 微基准上以 PRE+VR 运行，验证 VR 代码路径被真正执行：
#   vrRoundsEntered      -- 步幅检测在 PRE 期间达到置信度 3 并进入 VR；
#   vrGathersIssued      -- 触发 load 被向量化为 N 通道 gather；
#   vrPrefetchesIssued   -- gather 预取被 L1D 接受；
#   vrPrefetchesCompleted-- 预取响应正确回收（source 响应触发链回放）；
#   vrTaintedInstructions-- 依赖链上的指令被标记并记录。
# 同时断言配置生效与程序正常退出。
#
# 用法（服务器上）：
#   ROOT=/path/to/gem5-runahead-dev-pre \
#   RESULT_ROOT=/path/to/results/vr-stage1 \
#   bash scripts/run_remote_vr_stage1_smoke.sh

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
RESULT_ROOT="${RESULT_ROOT:-$HOME/dvr-repro/results/vr-stage1-smoke}"
PYTHON_ROOT="${PYTHON_ROOT:-/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15}"

export LD_LIBRARY_PATH="$PYTHON_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

read_stat() {
    awk -v name="$2" '$1 == name { print $2; exit }' "$1"
}
require_positive() {
    local label="$1" value="$2"
    if [[ -z "$value" || "$value" -le 0 ]]; then
        printf 'error: expected %s > 0, got %s\n' \
            "$label" "${value:-<missing>}" >&2
        exit 1
    fi
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

mkdir -p "$RESULT_ROOT"
rm -f "$RESULT_ROOT"/stats.txt "$RESULT_ROOT"/config.ini "$RESULT_ROOT"/run.log

"$gem5" -d "$RESULT_ROOT" "$ROOT/configs/example/se.py" \
    --cpu-type=DerivO3CPU --caches --cmd="$binary" \
    --param 'system.cpu[0].enablePRE=True' \
    --param 'system.cpu[0].enableVR=True' \
    >"$RESULT_ROOT/run.log" 2>&1

grep -q '^enablePRE=true$' "$RESULT_ROOT/config.ini"
grep -q '^enableVR=true$' "$RESULT_ROOT/config.ini"
grep -q 'exiting with last active thread context' "$RESULT_ROOT/run.log"

stats="$RESULT_ROOT/stats.txt"
rounds="$(read_stat "$stats" system.cpu.vrRoundsEntered)"
gathers="$(read_stat "$stats" system.cpu.vrGathersIssued)"
issued="$(read_stat "$stats" system.cpu.vrPrefetchesIssued)"
completed="$(read_stat "$stats" system.cpu.vrPrefetchesCompleted)"
tainted="$(read_stat "$stats" system.cpu.vrTaintedInstructions)"

require_positive vrRoundsEntered "$rounds"
require_positive vrGathersIssued "$gathers"
require_positive vrPrefetchesIssued "$issued"
require_positive vrPrefetchesCompleted "$completed"
require_positive vrTaintedInstructions "$tainted"

printf 'VR_STAGE1_SMOKE_PASSED rounds=%s gathers=%s issued=%s completed=%s tainted=%s\n' \
    "$rounds" "$gathers" "$issued" "$completed" "$tainted"
