#!/usr/bin/env bash
set -euo pipefail

# Fixed-input Camel cache-capacity study. Only --l1d-size varies between rows.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
CAMEL_SRC="${CAMEL_SRC:-/home/lynnhoo/gem-test/gem5-leap/leap-bench/hpc/camel/camel.c}"
CC="${CC:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-gcc}"
M5_LIB="${M5_LIB:-$ROOT/util/m5/build/riscv/out/libm5.a}"
OBJDUMP="${OBJDUMP:-/home/lynnhoo/buckyball/result/bin/riscv64-unknown-linux-gnu-objdump}"
MAX_KEY="${MAX_KEY:-65536}"
SIZES="${SIZES:-16KiB,32KiB}"
OUT="${OUT:-/home/lynnhoo/dvr-repro/results/camel-hot-pc-cache-sweep-$(date +%Y%m%dT%H%M%S)}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
first_stat() {
    awk -v key="$2" '$1 == key { print $2; exit }' "$1"
}
pc_counts() {
    awk -F, -v pc="$2" '$2 == pc {print $3 "," $4; exit}' "$1"
}

[[ -x "$GEM5" && -f "$CONFIG" && -x "$CC" && -x "$OBJDUMP" && -s "$M5_LIB" ]] || die "missing build dependency"
mkdir -p "$OUT/benchmarks"
BENCH="$OUT/benchmarks/camel.riscv"
"$CC" -O2 -fno-tree-vectorize -static -DMAX_KEY="$MAX_KEY" \
    -DENABLE_GEM5_STATS -I"$ROOT/include" -o "$BENCH" "$CAMEL_SRC" "$M5_LIB"

# These are derived once from the fixed ELF. They are the source ld 0(s1) and
# immediately following dependent lw 0(a4) in Camel's hot loop.
source_pc="$("$OBJDUMP" -d "$BENCH" | awk '!found && /ld[[:space:]]+a4,0\(s1\)/ {gsub(":", "", $1); result="0x" $1; found=1} END {print result}')"
[[ -n "$source_pc" ]] || die "could not locate Camel source load PC"
dependent_pc="$("$OBJDUMP" -d "$BENCH" | awk -v source="$source_pc" '
    $1 ~ /:$/ {pc="0x" substr($1, 1, length($1)-1)}
    pc == source {seen=1; next}
    seen && !found && /lw[[:space:]]+a4,0\(a4\)/ {result=pc; found=1}
    END {print result}
')"
[[ -n "$dependent_pc" ]] || die "could not locate Camel dependent load PC"

printf '%s\n' 'l1d_size,mode,source_pc,dependent_pc,roi_demand_accesses,roi_demand_misses,total_l1d_miss_rate,source_misses,dependent_misses,hot_misses,hot_misses_per_l1d_access,hot_fraction_of_all_misses,source_miss_rate,dependent_miss_rate,sim_ticks,ipc,translation_faults' > "$OUT/camel_hot_pc_cache_sweep.csv"

IFS=',' read -r -a size_list <<< "$SIZES"
for size in "${size_list[@]}"; do
    for mode in Baseline DVR; do
        case_dir="$OUT/${size}/${mode}"
        mkdir -p "$case_dir"
        args=(--l1d-size="$size")
        if [[ "$mode" == DVR ]]; then
            args+=(--dvr --dvr-mode=nested --dvr-vector-chunks)
        fi
        DVR_PC_SUMMARY_DIR="$case_dir" "$GEM5" --outdir="$case_dir" "$CONFIG" \
            --cmd="$BENCH" "${args[@]}" > "$case_dir/stdout.log" 2>&1
        stats="$case_dir/stats.txt"
        dcache="$case_dir/cache_pc_system_cpu_dcache.csv"
        [[ -s "$stats" && -s "$dcache" ]] || die "missing result for $size/$mode"
        roi_misses="$(first_stat "$stats" system.cpu.dcache.architecturalDemandMisses)"
        roi_accesses="$(first_stat "$stats" system.cpu.dcache.architecturalDemandAccesses)"
        roi_hits="$(first_stat "$stats" system.cpu.dcache.architecturalDemandHits)"
        read -r pc_hits pc_misses < <(
            awk -F, 'NR > 1 {hits += $3; misses += $4}
                END {print hits + 0, misses + 0}' "$dcache"
        )
        [[ "$pc_hits" == "$roi_hits" && "$pc_misses" == "$roi_misses" ]] ||
            die "ROI/PC cache accounting mismatch for $size/$mode: stats=$roi_hits/$roi_misses pc=$pc_hits/$pc_misses"
        source="$(pc_counts "$dcache" "$source_pc")"
        dependent="$(pc_counts "$dcache" "$dependent_pc")"
        source_hits="${source%,*}"; source_misses="${source#*,}"
        dependent_hits="${dependent%,*}"; dependent_misses="${dependent#*,}"
        hot=$((source_misses + dependent_misses))
        awk -v size="$size" -v mode="$mode" -v source_pc="$source_pc" -v dependent_pc="$dependent_pc" \
            -v accesses="$roi_accesses" -v roi="$roi_misses" -v sm="$source_misses" -v dm="$dependent_misses" \
            -v sh="$source_hits" -v dh="$dependent_hits" \
            -v ticks="$(first_stat "$stats" simTicks)" -v ipc="$(first_stat "$stats" system.cpu.ipc)" \
            -v faults="$(first_stat "$stats" system.cpu.dvrPrefetchTranslationFaults)" \
            'BEGIN { hot=sm+dm; printf "%s,%s,%s,%s,%d,%d,%.6f,%d,%d,%d,%.6f,%.6f,%.6f,%.6f,%s,%s,%s\n", size, mode, source_pc, dependent_pc, accesses, roi, (accesses ? roi/accesses : 0), sm, dm, hot, (accesses ? hot/accesses : 0), (roi ? hot/roi : 0), ((sh+sm) ? sm/(sh+sm) : 0), ((dh+dm) ? dm/(dh+dm) : 0), ticks, ipc, faults }' \
            >> "$OUT/camel_hot_pc_cache_sweep.csv"
    done
done

printf 'CAMEL_HOT_PC_CACHE_SWEEP_COMPLETE out=%s source=%s dependent=%s\n' \
    "$OUT" "$source_pc" "$dependent_pc"
