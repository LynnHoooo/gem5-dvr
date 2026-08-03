#!/usr/bin/env bash
set -euo pipefail

# Run the two benchmark sources supplied by gem5-leap. BUILD=1 builds direct
# single-process ELFs; the qemu-linux wrappers use fork(), which is not
# supported by gem5 SE in this validation path.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BENCH_ROOT="${BENCH_ROOT:-/home/lynnhoo/gem-test/gem5-leap/leap-bench}"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/leap-bfs-camel-smoke}"
BFS="${BFS:-$BENCH_ROOT/build/leap_bfs_qemu-linux}"
CAMEL="${CAMEL:-$BENCH_ROOT/build/leap_camel_qemu-linux}"
BUILD="${BUILD:-0}"
REQUIRE_DVR="${REQUIRE_DVR:-1}"
BFS_SCALE="${BFS_SCALE:-6}"
CAMEL_MAX_KEY="${CAMEL_MAX_KEY:-1024}"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
read_stat() { awk -v name="$2" '$1 == name {print $2; exit}' "$1"; }

[[ -x "$GEM5" ]] || die "gem5 binary not executable: $GEM5"
[[ -f "$CONFIG" ]] || die "gem5 config not found: $CONFIG"
[[ -d "$BENCH_ROOT" ]] || die "benchmark root not found: $BENCH_ROOT"

if [[ "$BUILD" == 1 ]]; then
    [[ -n "${LINUX_CC:-}" && -n "${LINUX_CXX:-}" ]] || \
        die 'BUILD=1 requires LINUX_CC and LINUX_CXX'
    [[ -x "$LINUX_CC" && -x "$LINUX_CXX" ]] || \
        die 'LINUX_CC/LINUX_CXX must name executable RISC-V Linux compilers'
    BUILD_DIR="${BUILD_DIR:-$BENCH_ROOT/.dvr-smoke-build}"
    mkdir -p "$BUILD_DIR"
    "$LINUX_CXX" -g -std=c++11 -O3 -static -march=rv64gc -mcmodel=medany \
        -fno-tree-vectorize -I"$BENCH_ROOT/gap/src" \
        -I"$BENCH_ROOT/gap/benchmark" -DGAP_BFS_ALPHA=15 -DGAP_BFS_BETA=18 \
        -o "$BUILD_DIR/leap_bfs_direct.riscv" "$BENCH_ROOT/gap/src/bfs.cc"
    "$LINUX_CC" -g -O3 -static -march=rv64gc -mcmodel=medany \
        -DMAX_KEY="$CAMEL_MAX_KEY" \
        -o "$BUILD_DIR/leap_camel_direct.riscv" "$BENCH_ROOT/hpc/camel/camel.c" -lm
    BFS="$BUILD_DIR/leap_bfs_direct.riscv"
    CAMEL="$BUILD_DIR/leap_camel_direct.riscv"
fi

[[ -x "$BFS" ]] || die "BFS ELF not executable: $BFS (set BFS=... or use BUILD=1)"
[[ -x "$CAMEL" ]] || die "Camel ELF not executable: $CAMEL (set CAMEL=... or use BUILD=1)"

mkdir -p "$OUT_ROOT"
{
    printf 'repo=%s\n' "$REPO_ROOT"
    printf 'bench_root=%s\n' "$BENCH_ROOT"
    printf 'gem5=%s\n' "$GEM5"
    printf 'gem5_sha256=%s\n' "$(sha256sum "$GEM5" | awk '{print $1}')"
    printf 'bfs=%s\n' "$BFS"
    printf 'bfs_sha256=%s\n' "$(sha256sum "$BFS" | awk '{print $1}')"
    printf 'camel=%s\n' "$CAMEL"
    printf 'camel_sha256=%s\n' "$(sha256sum "$CAMEL" | awk '{print $1}')"
    printf 'git_sha=%s\n' "$(git -C "$REPO_ROOT" rev-parse HEAD)"
} > "$OUT_ROOT/manifest.txt"

run_one() {
    local name="$1" elf="$2" mode="$3"
    local out="$OUT_ROOT/$name/$mode"
    local options=""
    if [[ "$name" == bfs ]]; then
        options="-g $BFS_SCALE -n 1 -k 8 -a -v"
    fi
    mkdir -p "$out"
    if [[ "$mode" == baseline ]]; then
        "$GEM5" --outdir="$out" "$CONFIG" --cmd="$elf" \
            --options="$options" \
            >"$out/stdout.log" 2>&1
    else
        "$GEM5" --outdir="$out" "$CONFIG" --cmd="$elf" --dvr \
            --dvr-mode=nested --dvr-quality-probe \
            --options="$options" \
            >"$out/stdout.log" 2>&1
    fi
    [[ -s "$out/stats.txt" ]] || die "$name/$mode did not produce stats.txt"
    case "$name" in
        bfs)
            grep -q 'BFS Tree has' "$out/stdout.log" || die "$name/$mode did not finish BFS"
            ! grep -Eq 'Source wrong|Wrong depths|Couldn.t find edge|Reachability mismatch' \
                "$out/stdout.log" || die "$name/$mode BFS verifier failed"
            ;;
        camel)
            grep -q 'Result ' "$out/stdout.log" || die "$name/$mode did not finish Camel"
            ;;
    esac
}

run_one bfs "$BFS" baseline
run_one bfs "$BFS" nested
run_one camel "$CAMEL" baseline
run_one camel "$CAMEL" nested

printf 'workload,mode,simTicks,stride_candidates,discovery_starts,helper_issue_cycles,source_issued,source_completed,demand_addresses\n' \
    > "$OUT_ROOT/summary.csv"
for workload in bfs camel; do
    for mode in baseline nested; do
        stats="$OUT_ROOT/$workload/$mode/stats.txt"
        values=(
            "$(read_stat "$stats" simTicks)"
            "$(read_stat "$stats" system.cpu.dvrStrideCandidates)"
            "$(read_stat "$stats" system.cpu.dvrDiscoveryStarts)"
            "$(read_stat "$stats" system.cpu.dvrHelperIssueCycles)"
            "$(read_stat "$stats" system.cpu.dvrSourcePrefetchesIssued)"
            "$(read_stat "$stats" system.cpu.dvrSourcePrefetchesCompleted)"
            "$(read_stat "$stats" system.cpu.dvrQualityDemandAddressesObserved)"
        )
        printf '%s,%s,%s\n' "$workload" "$mode" "$(IFS=,; printf '%s' "${values[*]}")" \
            >> "$OUT_ROOT/summary.csv"
    done
done

for workload in bfs camel; do
    stats="$OUT_ROOT/$workload/nested/stats.txt"
    if [[ "$REQUIRE_DVR" == 1 ]]; then
        candidates="$(read_stat "$stats" system.cpu.dvrStrideCandidates)"
        starts="$(read_stat "$stats" system.cpu.dvrDiscoveryStarts)"
        [[ "${candidates:-0}" -gt 0 ]] || die "$workload nested produced no stride candidates"
        [[ "${starts:-0}" -gt 0 ]] || die "$workload nested produced no Discovery starts"
    fi
done

cat "$OUT_ROOT/summary.csv"
printf 'DVR_LEAP_BFS_CAMEL_SMOKE_PASSED out=%s\n' "$OUT_ROOT"
