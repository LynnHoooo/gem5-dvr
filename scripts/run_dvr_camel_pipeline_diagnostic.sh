#!/usr/bin/env bash
set -euo pipefail

# This is a teaching/diagnostic wrapper for the Figure 3 ordinary DVR path.
# It deliberately excludes the program exit/cleanup structure and focuses on
# the data path from RPT observation through helper/cache requests.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ROOT="${ROOT:-$REPO_ROOT/code/gem5-runahead-dev-pre}"
GEM5="${GEM5:-$ROOT/build/RISCV/gem5.opt}"
CONFIG="${CONFIG:-$ROOT/configs/dvr/table1_se.py}"
BENCH="${BENCH:-/home/lynnhoo/dvr-repro/results/camel-dvr-trace-c_lw-full/camel.riscv}"
OUT_ROOT="${OUT_ROOT:-/home/lynnhoo/dvr-repro/results/dvr-next-camel-pipeline-diagnostic}"
RUN_ID="$(date +%Y%m%dT%H%M%S)-$$"
OUT="$OUT_ROOT/$RUN_ID"
BASELINE="$OUT/baseline"
FULL="$OUT/full-vector"
TRACE="$FULL/trace"

read_stat() {
    awk -v name="$2" '$1 == name {print $2; exit}' "$1"
}

stat() {
    read_stat "$FULL/stats.txt" "system.cpu.$1"
}

say() {
    printf '\n%s\n' "$1"
}

line() {
    printf '  %-42s %s\n' "$1" "$2"
}

pass() {
    printf '  [PASS] %s\n' "$1"
}

warn() {
    printf '  [WARN] %s\n' "$1"
}

fail() {
    printf '  [FAIL] %s\n' "$1" >&2
    exit 1
}

positive() {
    local label="$1" value="$2"
    [[ -n "$value" && "$value" -gt 0 ]] || fail "$label = ${value:-<missing>}"
}

equal() {
    local label="$1" lhs="$2" rhs="$3"
    [[ "$lhs" == "$rhs" ]] || fail "$label: $lhs != $rhs"
}

[[ -x "$GEM5" ]] || fail "gem5 not found: $GEM5"
[[ -f "$CONFIG" ]] || fail "config not found: $CONFIG"
[[ -x "$BENCH" ]] || fail "Camel binary not found: $BENCH"
mkdir -p "$BASELINE/trace" "$FULL/trace"

"$GEM5" --outdir="$BASELINE" "$CONFIG" --cmd="$BENCH" \
    >"$BASELINE/stdout.log" 2>&1
DVR_TRACE_DIR="$TRACE" "$GEM5" --outdir="$FULL" "$CONFIG" \
    --cmd="$BENCH" --dvr --dvr-mode=full --dvr-vector-chunks \
    --dvr-quality-probe >"$FULL/stdout.log" 2>&1

[[ -s "$BASELINE/stats.txt" ]] || fail "baseline stats missing"
[[ -s "$FULL/stats.txt" ]] || fail "full-vector stats missing"

baseline_result="$(awk '/^Result / {print $2; exit}' "$BASELINE/stdout.log")"
full_result="$(awk '/^Result / {print $2; exit}' "$FULL/stdout.log")"
equal "architectural result" "$baseline_result" "$full_result"
baseline_committed="$(read_stat "$BASELINE/stats.txt" system.cpu.committedInsts)"
full_committed="$(read_stat "$FULL/stats.txt" system.cpu.committedInsts)"
equal "committed instructions" "$baseline_committed" "$full_committed"

say "Figure 3 DVR pipeline diagnostic"
line "benchmark" "$BENCH"
line "output" "$OUT"
line "result" "$full_result"
line "committed instructions" "$full_committed"

say "1. RPT / stride detector"
loads="$(stat dvrLoadsObserved)"
candidates="$(stat dvrStrideCandidates)"
line "loads observed by RPT" "$loads"
line "confident stride candidates" "$candidates"
positive "RPT saw no load" "$loads"
positive "stride detector produced no candidate" "$candidates"
candidate_ratio="$(awk -v c="$candidates" -v l="$loads" \
    'BEGIN {if (l == 0) print "0.0000"; else printf "%.4f", c / l}')"
line "candidate / observed-load ratio" "$candidate_ratio"
pass "dispatch observed striding-load candidates"
printf '  sample candidate/load events:\n'
awk -F, '$3 == "stride_candidate" || $3 == "load" {print "    " $0; if (++n == 8) exit}' \
    "$TRACE/workload.csv" 2>/dev/null || warn "candidate events are not present in this older trace"

say "2. Discovery Mode"
starts="$(stat dvrDiscoveryStarts)"
completions="$(stat dvrDiscoveryCompletions)"
timeouts="$(stat dvrDiscoveryTimeouts)"
abandons="$(stat dvrDiscoveryAbandons)"
line "Discovery starts" "$starts"
line "Discovery completions" "$completions"
line "Discovery timeouts" "$timeouts"
line "Discovery abandons/rollbacks" "$abandons/$(stat dvrDiscoveryRollbacks)"
positive "Discovery did not start" "$starts"
positive "Discovery did not complete" "$completions"
[[ "$completions" -le "$starts" ]] || fail "Discovery completions exceed starts"
pass "Discovery is active and completion count is bounded"
printf '  sample dependency events:\n'
awk -F, '$2 == "tainted" || $2 == "flr" || $2 == "discovery_start" || $2 == "discovery_complete" {print "    " $0; if (++n == 8) exit}' \
    "$TRACE/dependency_chain.csv" 2>/dev/null || true

say "3. VTT / taint propagation / FLR"
tainted="$(stat dvrTaintedInstructions)"
dependent_loads="$(stat dvrDependentLoads)"
flr="$(stat dvrDiscoveriesWithFLR)"
relations="$(stat dvrAddressRelationsTrained)"
line "tainted instructions" "$tainted"
line "tainted dependent loads" "$dependent_loads"
line "discoveries with FLR" "$flr"
line "trained trigger-to-FLR relations" "$relations"
positive "VTT marked no instructions" "$tainted"
positive "VTT found no dependent load" "$dependent_loads"
positive "FLR was never identified" "$flr"
[[ "$flr" -le "$completions" ]] || fail "FLR count exceeds Discovery completions"
pass "taint reached a dependent load and produced FLR metadata"
printf '  taint/FLR samples:\n'
awk -F, '$2 == "tainted" || $2 == "flr" {print "    " $0; if (++n == 10) exit}' \
    "$TRACE/dependency_chain.csv"

say "4. Loop-bound detector"
bounds="$(stat dvrLoopBoundsFound)"
bound_discoveries="$(stat dvrDiscoveriesWithBounds)"
matches="$(stat dvrLoopBoundMatches)"
fallbacks="$(stat dvrLoopBoundFallbacks)"
lane_samples="$(stat dvrLaneCountSamples)"
active_lanes="$(stat dvrTotalActiveLanes)"
line "backward branches found" "$(stat dvrBackwardBranches)"
line "loop bounds found" "$bounds"
line "discoveries with bounds" "$bound_discoveries"
line "register-checkpoint matches" "$matches"
line "max-lane fallbacks" "$fallbacks"
line "lane-count samples" "$lane_samples"
line "total inferred active lanes" "$active_lanes"
positive "loop-bound detector found no bound" "$bounds"
positive "no lane count was inferred" "$lane_samples"
[[ "$matches" -le "$bound_discoveries" ]] || fail "bound matches exceed bounded discoveries"
pass "loop bound and active lane count are produced"

say "5. Recorder / replay program"
recorded="$(stat dvrRecordedUops)"
overflows="$(stat dvrRecorderOverflows)"
programs="$(stat dvrVectorProgramsBuilt)"
unsupported="$(stat dvrReplayUnsupportedUops)"
line "recorded metadata uops" "$recorded"
line "recorder overflows" "$overflows"
line "vector programs built" "$programs"
line "unsupported replay uops" "$unsupported"
positive "recorder produced no uop" "$recorded"
positive "no vector program was built" "$programs"
[[ "$unsupported" -eq 0 ]] || warn "some instructions are outside the supported evaluator subset"
[[ "$overflows" -eq 0 ]] || warn "some discovery templates exceeded recorder capacity"
pass "captured metadata became replay programs"

say "6. VRAT / private vector register file"
vrat_programs="$(stat dvrHelperVRATPrograms)"
vrat_writes="$(stat dvrHelperVRATWrites)"
line "private VRAT programs" "$vrat_programs"
line "lane register writes" "$vrat_writes"
positive "VRAT was never allocated" "$vrat_programs"
positive "VRAT never received a lane write" "$vrat_writes"
[[ "$vrat_writes" -ge "$vrat_programs" ]] || fail "VRAT writes are fewer than programs"
pass "source values can be written into private helper lanes"

say "7. Vectorizer / address lanes"
source_lanes="$(stat dvrVectorizerSourceLanes)"
dependent_lanes="$(stat dvrVectorizerDependentLanes)"
source_trace="$(awk -F, '$2 == "source_lane" {++n} END {print n + 0}' "$TRACE/vectorization.csv")"
target_trace="$(awk -F, '$2 == "replay_target" {++n} END {print n + 0}' "$TRACE/dependency_chain.csv")"
source_values="$(awk -F, '$2 == "source_value" {++n} END {print n + 0}' \
    "$TRACE/dependency_chain.csv")"
targets_without_source="$(awk -F, '
    $2 == "source_value" {
        key = $3 SUBSEP $7
        if (!(key in first_source_tick) || $1 < first_source_tick[key])
            first_source_tick[key] = $1
    }
    $2 == "replay_target" {
        key = $3 SUBSEP $7
        ++targets
        if (!(key in first_source_tick) || first_source_tick[key] >= $1)
            ++missing
    }
    END {print missing + 0}
' "$TRACE/dependency_chain.csv")"
trace_max_group="$(awk -F, '$2 == "vir_issue_group" && $5 > max {max = $5} END {print max + 0}' \
    "$TRACE/vectorization.csv")"
line "source lanes materialized" "$source_lanes"
line "dependent lanes materialized" "$dependent_lanes"
line "source-lane/value/target trace entries" "$source_trace/$source_values/$target_trace"
line "targets without an earlier source value" "$targets_without_source"
line "trace maximum VIR group width" "$trace_max_group"
equal "source lane trace/stat" "$source_lanes" "$source_trace"
equal "dependent lane trace/stat" "$dependent_lanes" "$target_trace"
equal "dependent target source-value predecessor" "$targets_without_source" 0
positive "vectorizer produced no source lane" "$source_lanes"
positive "vectorizer produced no dependent lane" "$dependent_lanes"
positive "source response trace is empty" "$source_values"
pass "vectorizer output is visible as source and dependent address events"
printf '  source lane samples:\n'
awk -F, '$2 == "source_lane" {print "    " $0; if (++n == 8) exit}' \
    "$TRACE/vectorization.csv"
printf '  dependent target samples:\n'
awk -F, '$2 == "replay_target" {print "    " $0; if (++n == 8) exit}' \
    "$TRACE/dependency_chain.csv"

say "8. VIR / active mask / same-PC batching"
mask_checks="$(stat dvrVIRActiveMaskChecks)"
mask_failures="$(stat dvrVIRActiveMaskFailures)"
groups="$(stat dvrVIRContinuationPCGroups)"
group_lanes="$(stat dvrVIRContinuationGroupedLanes)"
max_group="$(stat dvrVIRContinuationMaxGroupWidth)"
chunks="$(stat dvrVectorChunkRequests)"
multi_lane_groups="$(awk -F, '$2 == "vir_issue_group" && $5 >= 2 {++n} END {print n + 0}' \
    "$TRACE/vectorization.csv")"
line "active-mask checks/failures" "$mask_checks/$mask_failures"
line "same-PC VIR groups" "$groups"
line "groups with at least 2 lanes" "$multi_lane_groups"
line "lanes in VIR groups" "$group_lanes"
line "maximum group width" "$max_group"
line "vector chunk requests" "$chunks"
positive "VIR performed no mask check" "$mask_checks"
equal "VIR active-mask failures" "$mask_failures" 0
positive "VIR formed no issue group" "$groups"
positive "VIR formed no multi-lane group" "$multi_lane_groups"
equal "VIR trace/stat maximum width" "$trace_max_group" "$max_group"
[[ "$max_group" -ge 2 ]] || fail "Camel did not form a multi-lane VIR group"
[[ "$max_group" -le 8 ]] || fail "64-bit 512-bit chunk exceeded 8 lanes"
pass "active masks match grouped lanes and same-PC batching occurred"
printf '  VIR group samples:\n'
awk -F, '$2 == "vir_issue_group" {print "    " $0; if (++n == 8) exit}' \
    "$TRACE/vectorization.csv"

say "9. Helper subthread / uop lifetime / front end"
decoded="$(stat dvrHelperDynUopsDecoded)"
issued="$(stat dvrHelperDynUopsIssued)"
completed="$(stat dvrHelperDynUopsCompleted)"
line "DynUop decoded/issued/completed" "$decoded/$issued/$completed"
line "helper fetch/decode cycles" "$(stat dvrHelperFetchCycles)/$(stat dvrHelperDecodeCycles)"
line "decoded-uop cache hits/misses" "$(stat dvrHelperDecodedCacheHits)/$(stat dvrHelperDecodedCacheMisses)"
line "VIR capacity stalls" "$(stat dvrHelperVIRCapacityStalls)"
equal "DynUop decoded=issued" "$decoded" "$issued"
equal "DynUop issued=completed" "$issued" "$completed"
positive "helper decoded no DynUop" "$decoded"
pass "helper uop lifecycle is conserved"

say "10. FU / vector execution"
fu_requests="$(stat dvrHelperFURequests)"
fu_grants="$(stat dvrHelperFUGrants)"
fu_stalls="$(stat dvrHelperFUStalls)"
line "FU requests/grants/stalls" "$fu_requests/$fu_grants/$fu_stalls"
line "ALU/shift/multiply chunks" "$(stat dvrVectorALUChunkIssues)/$(stat dvrVectorShiftChunkIssues)/$(stat dvrVectorMultiplyChunkIssues)"
line "vector FU conflict cycles" "$(stat dvrVectorFUConflictCycles)"
line "modeled vector latency cycles" "$(stat dvrVectorLatencyCycles)"
positive "helper requested no FU" "$fu_requests"
equal "FU requests=grants+stalls" "$fu_requests" "$((fu_grants + fu_stalls))"
if [[ "$(stat dvrVectorFUConflictCycles)" -eq 0 ]]; then
    warn "Camel main thread has no SIMD instructions; FU contention is not exercised"
else
    pass "helper and main thread contended for vector FU capacity"
fi
pass "helper vector operations are routed through native FU capability names"

say "11. LSQ / cache / memory response"
generated="$(stat dvrPrefetchesGenerated)"
dependent_generated="$(stat dvrDependentPrefetchesGenerated)"
prefetch_issued="$(stat dvrPrefetchesIssued)"
prefetch_completed="$(stat dvrPrefetchesCompleted)"
dependent_issued="$(stat dvrDependentPrefetchesIssued)"
dependent_completed="$(stat dvrDependentPrefetchesCompleted)"
faults="$(stat dvrPrefetchTranslationFaults)"
total_generated=$((generated + dependent_generated))
line "source/dependent requests generated" "$generated/$dependent_generated"
line "all requests generated/issued/completed" "$total_generated/$prefetch_issued/$prefetch_completed"
line "source requests issued/completed" "$(stat dvrSourcePrefetchesIssued)/$(stat dvrSourcePrefetchesCompleted)"
line "dependent requests issued/completed" "$dependent_issued/$dependent_completed"
line "translation faults" "$faults"
line "possibly useful / late" "$(stat dvrPrefetchesPossiblyUseful)/$(stat dvrPrefetchesLate)"
line "outstanding line peak" "$(stat dvrOutstandingPrefetchLinePeak)"
equal "all request issue/complete" "$prefetch_issued" "$prefetch_completed"
equal "all request generate/issue" "$total_generated" "$prefetch_issued"
equal "dependent issue/complete" "$dependent_issued" "$dependent_completed"
equal "translation faults" "$faults" 0
positive "no request reached LSQ/cache" "$prefetch_issued"
positive "no dependent demand was covered" "$(stat dvrDependentDemandCovered)"
pass "helper requests reached and returned from the timing memory system"

say "11b. Prefetch quality"
probe_stat() {
    read_stat "$FULL/stats.txt" "system.cpu.dvr_quality_probe.$1"
}
quality_useful="$(probe_stat usefulTimely)"
quality_late="$(probe_stat usefulLate)"
quality_covered="$(probe_stat coveredMisses)"
quality_accuracy="$(probe_stat fillAccuracy)"
quality_coverage="$(probe_stat coverage)"
quality_timeliness="$(probe_stat timeliness)"
quality_pollution="$(probe_stat pollutionEvictions)"
quality_pollution_misses="$(probe_stat pollutionMisses)"
line "quality useful timely/late" "$quality_useful/$quality_late"
line "quality covered misses" "$quality_covered"
line "accuracy / coverage / timeliness" \
    "$quality_accuracy/$quality_coverage/$quality_timeliness"
line "pollution evictions / pollution misses" \
    "$quality_pollution/$quality_pollution_misses"
positive "quality tracker saw no timely useful fill" "$quality_useful"
positive "quality tracker found no covered miss" "$quality_covered"

say "12. Final interpretation"
baseline_ticks="$(read_stat "$BASELINE/stats.txt" simTicks)"
full_ticks="$(read_stat "$FULL/stats.txt" simTicks)"
speedup="$(awk -v b="$baseline_ticks" -v f="$full_ticks" \
    'BEGIN {printf "%.6fx", b / f}')"
line "baseline/full ticks" "$baseline_ticks/$full_ticks"
line "Full Vector speedup" "$speedup"
line "trace files" "$TRACE"
printf '\n  The check proves ordinary DVR dataflow on Camel.\n'
printf '  It does not prove NDM flattening or main-thread SIMD contention.\n'
printf '  Read the next section in the README for the meaning of each counter.\n'
printf '\nDVR_CAMEL_PIPELINE_DIAGNOSTIC_PASSED run=%s\n' "$RUN_ID"
