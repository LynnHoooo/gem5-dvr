% DVR implementation and verification record
%
% This file is intentionally a plain, grep-friendly record even though the
% suffix is .m.  The authoritative source comparison is the paper at
% /home/lynnhoo/dvr-repro/decouple vector runahead.md.

STATUS = 'mechanism-level implementation; paper-level gaps remain';
BRANCH = 'docs/dvr-reproduction-plan';
LOCAL_HEAD = 'a8aaed2';
REMOTE_HEAD = 'b5642dd';
REMOTE_MERGED = true;

% Merge audit
% origin/docs/dvr-reproduction-plan (b5642dd) is an ancestor of de2f0bc.
% No cherry-pick was required.  User benchmark/scripts and the uncommitted
% helper VRAT edits were preserved.

% Paper-to-code mapping
% 1. Stride detector/VTT: implemented.  The detector identifies trigger PC,
%    stride and target architectural register; VTT/Discovery propagates taint.
% 2. VRAT: implemented in helper-private form.  All architectural integer
%    registers first map to helper scalar physical entries copied from the
%    committed register snapshot.  The trigger destination maps to 16 vector
%    copies, 8 x 64-bit elements each (128 scalar-equivalent lanes).
% 3. VIR: implemented as an execution-driven 16-copy model.  Each copy now
%    has active/issued/executed/dead-source masks and an in-flight count.
%    Vectorized sources cause a destination vector bundle allocation.
% 4. Helper frontend: independent timing instruction fetch/decode and an
%    eight-entry helper-local buffer are implemented; it is not O3 fetch/IQ.
% 5. Helper memory: MMU/data-port/cache requests use helper sender state and a
%    private 16-entry outstanding-load bound with retry and completion release.
% 6. Branch/reconvergence: per-lane PC, branch masks and bounded stack are
%    implemented.  NDM has branch inversion, outer invocation collection and
%    max-16 invocation/max-128 flattening.

% Explicit non-completions
% - VRAT is not mapped into the main O3 UnifiedFreeList/ROB/rename map.  This
%   is deliberate: the helper is an independent in-order subthread.  The
%   remaining work is exact dead-source reclamation and full scalar/vector WAW
%   rename behavior.
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
% BFS, serial GAPBS -g6 -n1:
%   graph completed (64 nodes, 390 edges), no translation fault observed;
%   baseline/full committed = 1207080/1207082;
%   full loop-bound matches/vector programs = 196/196;
%   dependent issued/completed = 592/592; active-mask failures = 0;
%   strict committed-instruction gate = NOT PASS (difference 2).
% Camel trace binary:
%   baseline/full program result = 2125659619/2125659619;
%   full helper/vector execution completed without translation fault;
%   baseline/full committed = 6874840/6874833;
%   strict committed-instruction gate = NOT PASS (difference 7).
%   A scalar helper run (without --dvr-vector-chunks) reproduced the baseline
%   committed count 6874840 and had zero translation faults; the vector-chunk
%   mode remains the failing gate.
%   After the dependent-prefetch gate fix, --dvr-no-dependent-prefetch reports
%   dependent issued/completed = 0/0 as required.
% Baseline repeat control:
%   Camel baseline repeated twice at 6874840/6874840, so the 7-instruction
%   vector-mode difference is deterministic and must not be ignored.
% LBD/VTT microbenchmark:
%   dvr_lbd_vtt completed with no alternate path observed; its run reached
%   the normal exit path and reported max same-PC group width 8.

% Evidence locations
% BFS result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-bfs-g6/
% Camel result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-camel/
% Microbenchmark result:
%   /home/lynnhoo/dvr-repro/results/vrat-vir-regression/
