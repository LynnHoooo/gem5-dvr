#!/usr/bin/env bash
set -euo pipefail

GEM5_ROOT="${GEM5_ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
GAP_ROOT="${GAP_ROOT:-$HOME/dvr-repro/source/gapbs}"
OUT_ROOT="${OUT_ROOT:-$HOME/dvr-repro/results/gap-bfs-s10}"
GAP_URL="https://github.com/sbeamer/gapbs.git"
GAP_SHA="2972aeb2703165bafd921222f4ed7196f542d3a8"
GAP_CXX="${GAP_CXX:-$HOME/buckyball/result/bin/riscv64-unknown-linux-gnu-g++}"

if [[ ! -d "$GAP_ROOT/.git" ]]; then
    git clone "$GAP_URL" "$GAP_ROOT"
fi
test "$(git -C "$GAP_ROOT" rev-parse HEAD)" = "$GAP_SHA"
test -x "$GAP_CXX"
make -B -C "$GAP_ROOT" SERIAL=1 CXX="$GAP_CXX" \
    CXX_FLAGS='-std=c++11 -O3 -Wall -static -fno-tree-vectorize' bfs
file "$GAP_ROOT/bfs" | grep -q 'RISC-V'

mkdir -p "$OUT_ROOT"
{
    printf 'gap_url=%s\n' "$GAP_URL"
    printf 'gap_sha=%s\n' "$GAP_SHA"
    printf 'compiler=%s\n' "$($GAP_CXX --version | head -1)"
    printf 'compiler_path=%s\n' "$GAP_CXX"
    printf 'compile_flags=%s\n' \
        '-std=c++11 -O3 -Wall -static -fno-tree-vectorize SERIAL=1'
    printf 'elf_sha256=%s\n' "$(sha256sum "$GAP_ROOT/bfs" | awk '{print $1}')"
    printf 'gem5_sha256=%s\n' \
        "$(sha256sum "$GEM5_ROOT/build/RISCV/gem5.opt" | awk '{print $1}')"
    printf 'input=%s\n' '-g 10 -n 1'
} >"$OUT_ROOT/manifest.txt"

read_stat() { awk -v n="$2" '$1 == n {print $2; exit}' "$1"; }
run_one() {
    local mode="$1" out="$OUT_ROOT/$1"
    local dvr_args=()
    if [[ "$mode" != baseline ]]; then
        dvr_args=(--dvr --dvr-mode="$mode" --dvr-quality-probe)
    fi
    mkdir -p "$out"
    "$GEM5_ROOT/build/RISCV/gem5.opt" --outdir="$out" \
        "$GEM5_ROOT/configs/dvr/table1_se.py" \
        --cmd="$GAP_ROOT/bfs" --options='-g 10 -n 1' \
        "${dvr_args[@]}" >"$out/stdout.log" 2>&1
    grep -q 'Graph has 1024 nodes and 10496 undirected edges' "$out/stdout.log"
    grep -q 'exiting with last active thread context' "$out/stdout.log"
}

run_one baseline
run_one full
run_one nested

printf 'mode,ticks,ipc,demand_misses,helper_generated,helper_issued,conflicts,nested_batches,fill_accuracy,coverage,timeliness,pollution_evictions\n' \
    >"$OUT_ROOT/summary.csv"
for mode in baseline full nested; do
    stats="$OUT_ROOT/$mode/stats.txt"
    values=(
        "$(read_stat "$stats" simTicks)"
        "$(read_stat "$stats" system.cpu.ipc)"
        "$(read_stat "$stats" system.cpu.dcache.demandMisses::total)"
        "$(read_stat "$stats" system.cpu.dvrPrefetchesGenerated)"
        "$(read_stat "$stats" system.cpu.dvrPrefetchesIssued)"
        "$(read_stat "$stats" system.cpu.dvrResourceConflicts)"
        "$(read_stat "$stats" system.cpu.dvrNestedFlattenBatches)"
        "$(read_stat "$stats" system.cpu.dvr_quality_probe.fillAccuracy)"
        "$(read_stat "$stats" system.cpu.dvr_quality_probe.coverage)"
        "$(read_stat "$stats" system.cpu.dvr_quality_probe.timeliness)"
        "$(read_stat "$stats" system.cpu.dvr_quality_probe.pollutionEvictions)"
    )
    printf '%s,%s\n' "$mode" "$(IFS=,; echo "${values[*]}")" \
        >>"$OUT_ROOT/summary.csv"
done
cat "$OUT_ROOT/summary.csv"
echo DVR_GAP_BFS_S10_SMOKE_PASSED
