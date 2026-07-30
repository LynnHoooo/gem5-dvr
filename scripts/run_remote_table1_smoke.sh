#!/usr/bin/env bash
set -euo pipefail

ROOT="${ROOT:-$HOME/dvr-repro/source/gem5-runahead-dev-pre}"
BENCH="${BENCH:-$ROOT/benchmarks/dvr_stride.riscv}"
OUT="${OUT:-$HOME/dvr-repro/results/table1-baseline-smoke}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/build/RISCV/gem5.opt" --outdir="$OUT" \
    "$ROOT/configs/dvr/table1_se.py" --cmd="$BENCH" --dvr

cfg="$OUT/config.ini"
stats="$OUT/stats.txt"
test -s "$cfg"
test -s "$stats"

require_cfg() {
    local section="$1"
    local expression="$2"
    local description="$3"
    awk -v section="[$section]" -v expression="$expression" '
        $0 == section { inside = 1; next }
        /^\[/ { inside = 0 }
        inside && $0 ~ expression { found = 1 }
        END { exit !found }
    ' "$cfg" || {
        echo "TABLE1_CONFIG_CHECK_FAILED: $description" >&2
        exit 1
    }
}

require_cfg system.cpu.clk_domain '^clock=250$' 'CPU is not 4GHz'
for item in \
    'numROBEntries=350:ROB entries' 'numIQEntries=128:issue queue entries' \
    'LQEntries=128:load queue entries' 'SQEntries=72:store queue entries' \
    'fetchWidth=5:fetch width' 'decodeWidth=5:decode width' \
    'renameWidth=5:rename width' 'dispatchWidth=5:dispatch width' \
    'issueWidth=5:issue width' 'commitWidth=5:commit width' \
    'numPhysIntRegs=256:integer registers' \
    'numPhysFloatRegs=256:floating-point registers' \
    'numPhysVecRegs=128:vector registers' 'enableDVR=true:DVR switch'; do
    require_cfg system.cpu "^${item%%:*}$" "${item#*:}"
done
require_cfg system.cpu.branchPred '^type=TAGE_SC_L_8KB$' 'branch predictor'
require_cfg system.cpu.icache '^size=32768$' 'L1I size'
require_cfg system.cpu.icache '^assoc=4$' 'L1I associativity'
require_cfg system.cpu.icache '^tag_latency=2$' 'L1I latency'
require_cfg system.cpu.dcache '^size=32768$' 'L1D size'
require_cfg system.cpu.dcache '^assoc=8$' 'L1D associativity'
require_cfg system.cpu.dcache '^tag_latency=4$' 'L1D latency'
require_cfg system.cpu.dcache '^mshrs=24$' 'L1D MSHRs'
require_cfg system.cpu.dcache.prefetcher '^table_entries=16$' 'stride streams'
require_cfg system.l2 '^size=262144$' 'L2 size'
require_cfg system.l2 '^assoc=8$' 'L2 associativity'
require_cfg system.l2 '^tag_latency=8$' 'L2 latency'
require_cfg system.l3 '^size=8388608$' 'L3 size'
require_cfg system.l3 '^assoc=16$' 'L3 associativity'
require_cfg system.l3 '^tag_latency=30$' 'L3 latency'
require_cfg system.memory '^latency=50000$' 'memory latency'
require_cfg system.memory '^bandwidth=18[.]000000$' 'memory bandwidth'

echo "TABLE1_BASELINE_SMOKE_PASSED out=$OUT"
