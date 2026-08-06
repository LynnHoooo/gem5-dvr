% DVR implementation and verification record
%
% This file is intentionally a plain, grep-friendly record even though the
% suffix is .m.  The authoritative source comparison is the paper at
% /home/lynnhoo/dvr-repro/decouple vector runahead.md.

STATUS = 'mechanism-level implementation with paper-sized helper VRAT; strict workload gaps remain';
BRANCH = 'docs/dvr-reproduction-plan';
LOCAL_HEAD = 'working-tree (early-exit reset pending commit)';
REMOTE_HEAD = 'b5642dd';
REMOTE_MERGED = true;

% Merge audit
% origin/docs/dvr-reproduction-plan (b5642dd) is an ancestor of de2f0bc.
% No cherry-pick was required.  User benchmark/scripts and the uncommitted
% helper VRAT edits were preserved.

% Paper-to-code mapping
% 1. Stride detector/VTT: implemented.  The detector identifies trigger PC,
%    stride and target architectural register; VTT/Discovery propagates taint.
%    Loop-bound inference now rejects observed early exits and handles signed
%    induction deltas without unsigned wraparound.
% 2. VRAT: implemented in helper-private form with the paper-sized budget.
%    All architectural integer registers first map to helper scalar physical
%    entries copied from the committed register snapshot.  The trigger
%    destination maps to 16 vector copies, 8 x 64-bit elements each (128
%    scalar-equivalent lanes); the bank now has 256 scalar and 128 vector
%    physical names.  VIR issue captures physical source IDs and retirement
%    releases them, so overwritten names are deferred while still in use.
% 3. VIR: implemented as an execution-driven 16-copy model.  Each copy now
%    has active/issued/executed/dead-source masks and an in-flight count.
%    Vectorized sources cause a destination vector bundle allocation.
% 4. Helper frontend: independent timing instruction fetch/decode and an
%    eight-entry helper-local buffer are implemented; it is not O3 fetch/IQ.
% 5. Helper memory: MMU/data-port/cache requests use helper sender state and a
%    private 16-entry outstanding-load bound with retry and completion release.
%    Each request now has a helperLoadId and Allocated/Retry/WaitingResponse/
%    Writeback/Completed/Fault/Dropped state; response writeback and wakeup are
%    counted independently of the main DynInst/LQ lifecycle.
% 6. Branch/reconvergence: per-lane PC, branch masks and bounded stack are
%    implemented.  NDM has branch inversion, outer invocation collection and
%    max-16 invocation/max-128 flattening.

% Explicit non-completions
% - VRAT is not mapped into the main O3 UnifiedFreeList/ROB/rename map.  This
%   is deliberate: the helper is an independent in-order subthread.  Scalar
%   WAW renaming across divergent subsets still needs a dedicated per-copy
%   mapping; the current deferred-release path covers physical source lifetime
%   but does not claim full branch-subset rename equivalence.
% - VIR scheduling, helper LSU merge behavior, frontend retry timing and NDM
%   control flow are still execution-driven approximations, not bit-exact
%   paper hardware.
% - Alternate-path strict coverage is workload dependent and not proven by
%   the current precompiled dvr_divergent binary.

% Build verification (2026-08-06)
% Command:
%   cd code/gem5-runahead-dev-pre
%   scons -j2 build/RISCV/gem5.opt
% Result: PASS; gem5.opt linked successfully.

% Benchmark verification (2026-08-06)
% dvr_divergent.riscv:
%   baseline/full committed = 348204/348204
%   full dependent issued/completed = 400/400
%   helper decoded/issued/completed = 3702/3702/3702
%   active-mask failures = 0; unsupported semantic lanes = 0;
%   strict alternate-path gate = NOT PASS: this precompiled binary produced
%   complete alternate hits 0 and alternate targets 0.
%   Note: the binary recorded complete suffixes, but this run did not expose
%   the opposite branch as a cache lookup; it is not evidence of alternate
%   replay failure by itself.
% BFS, serial GAPBS -g6 -n1 (binary rebuilt after early-exit reset and VRAT
% physical-lifetime changes):
%   graph completed (64 nodes, 390 edges);
%   baseline/full committed = 1207080/1207082;
%   full loop-bound matches/vector programs = 196/196;
%   dependent issued/completed = 592/592; active-mask failures = 0;
%   helper load faults = 387 (source 359, dependent 28), so the translation-fault
%   gate is NOT PASS; resetting earlyExitSeen at each FLR did not change this count.
%   Alternate cache lookups/hits/complete-hits = 2179/75/0;
%   alternate uops/targets/demand-covered/resumes = 125/8/2/0.
%   Fault provenance shows 359 source lanes past the current graph allocation
%   and 28 dependent lanes at address 0x8; these are speculative fallback
%   addresses, not architectural faults.
%   strict committed-instruction gate = NOT PASS (difference 2).
% Camel trace binary (same rebuilt binary for both runs):
%   baseline/full program result = 2125659619/2125659619;
%   baseline/full committed = 6874814/6874814;
%   full helper/vector translation faults = 0; strict Camel committed gate
%   PASS for this run.  Older 6874840/6874833 results came from a different
%   generated Camel image/build and are retained only as historical evidence.
%   After the dependent-prefetch gate fix, --dvr-no-dependent-prefetch reports
%   dependent issued/completed = 0/0 as required.
% Baseline repeat control:
%   Camel baseline/full were rerun from the same generated image after the
%   paper-sized VRAT update; both committed counts are 6874814.
% LBD/VTT microbenchmark:
%   dvr_lbd_vtt completed with no alternate path observed; its run reached
%   the normal exit path and reported max same-PC group width 8.  Baseline/full
%   committed = 996495/996495, vector programs = 55, dependent issued/
%   completed = 33/33, translation faults = 0.  The helper-LQ peak was 16;
%   155584 entries were allocated, 124447 completed and 31123 retried before
%   the process exited, showing bounded retry behavior rather than overflow.
% NDM regression (dvr_nested-v2.riscv):
%   contexts/programs = 40050/13366; outer invocations = 13146; flatten
%   batches = 6573; flattened/expected lanes = 803056/803056; invariant
%   failures = 0; variable-lane batches = 4609.
%   Nested helper generated/issued/completed = 790594/738234/738234;
%   replay attempts/targets/fallbacks = 695824/42410/0.  Generated is below
%   flattened because duplicate addresses are removed before queueing.

% Evidence locations
% BFS result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-bfs-g6/
% Camel result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-camel/
% Microbenchmark result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-regression/
