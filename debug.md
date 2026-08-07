# DVR Debug Status

前半部分只保留当前未解决项；已解决内容、验证过程和历史结果统一放在末尾。

## 当前未解决

### P0 建模状态

本轮已关闭先前列出的 VRAT/VIR 执行数据面缺口：

1. 每个 vector destination/WAW 现在经 `renameVector()` 获取新的 16-copy bundle；已
   issue 的 source physical name 会保留到对应 DynUop retire。
2. `VIRCopyState` 已有独立 `readyMask` 与 `completedMask`；DynUop 完成时写入
   completed mask，并按 captured physical source name 释放。
3. `conservationValid()` 在 rename 与 retire 处断言 VRAT mapping 与 physical allocation
   一致，捕获 released mapping、悬空 user 和 pending-release 状态。
4. helper-private physical bank 现在是默认；`--dvr-shared-physical-bank` 仅作为显式
   sensitivity option。更重要的是 VRAT 已从长期 replay template 移至每次 helper launch
   的 execution context，历史 discovery 不会永久耗尽 private bank。
5. helper LSU 以每个 request 的 `readyTick` 作为 source/dependent 依赖门槛，不再因无关
   lane 的 frontend/replay 工作阻塞已 ready 的 dependent request。

这并不意味着 bit-exact Sniper reproduction：论文未公开所有仲裁细节，当前仍是
paper-faithful、独立 in-order helper timing model，而非第二条 O3/ROB 线程。

### P1：算法和 workload 覆盖

1. loop-bound detector 已覆盖显式 `slt/sltu + beq/bne`、fused `bne` 和
   `blt/bge/bltu/bgeu`，但复杂 compare、复杂 induction expression、中途进入、一般
   LCR/SBB 组合和带 live-out 的 early exit 仍可能 fallback，需要继续专项覆盖。

2. `LBU/LHU` 已进入构建，不再作为缺口；但更多 arithmetic、word operation、indirect
   control-flow、break/early-exit 和 path live-out 组合仍需专项测试。

3. BFS/Camel 的严格 alternate-path demand coverage 仍未完成。真实 serial GAPBS BFS
   `-g 11 -n 1` 已经产生 `complete_hits=2`、`alternate_uops=99`、
   `alternate_targets=1`，说明 cache-to-lane admission/replay 已接通；但该 target 的
   `alternate_demand_covered=0`、`reconvergence_resume=0`，所以 timeliness/路径恢复仍
   未被证明。当前 Camel 实际 workload 的普通 DVR 路径有效，但本次没有暴露 alternate
   complete path（`complete_hits=0`），不能沿用旧的 complete-hit 结果声称 Camel 已覆盖。

### P2：资源模型和论文复现

1. helper fetch/decode 带宽、主线程竞争、vector FU latency/throughput 和 LSQ contention
   仍需做 unlimited-FU、constrained-FU、主线程优先和 round-robin 敏感性实验。

2. 论文 workload、ROI、cache/MSHR/DRAM 配置尚未完全对齐 Sniper 原配置；需要先做
   NAS-IS、BFS/BC、PageRank 和短 inner-loop microbenchmark 的原配置锚定，再迁移到统一平台。

3. cache quality 仍需绑定完整的 L1 lookup/fill/eviction/victim/invalidate 事件，区分
   useful、late、evicted、pollution 和 demand coverage。

### 2026-08-07 M2 Nested data-plane 验收（当前 `bea09ec`）

在 `dvr_nested.riscv`、`--dvr-mode=nested --dvr-vector-chunks` 上，完整链已验证：

```text
flattened/expected lanes       877706 / 877706
flatten invariant failures     0
source responses completed     774935
nested replay attempts         774935
nested dependent targets       395215
dependent issued/completed     108887 / 108887
helper DynUop D/I/C            1037394 / 1037394 / 1037394
```

因此 M2 的“branch inversion -> outer invocation collection -> flatten -> source response
-> persistent replay -> dependent target -> cache request completion”已通过。target 数量大于
发射数量是有限 helper queue、deduplication、fault/drop 与主线程优先仲裁的预期结果；关键
守恒是 issued 等于 completed，且 flattened 等于 expected。

variable-inner-bound 回归同样通过：`dvrNestedVariableLaneBatches=1287`、target
generated `23358`、dependent issued/completed `4972/4972`、flatten failures `0`。

以下是本轮之前的历史失败记录，仅用于说明已修复的根因，不再代表当前状态。

| Gate | 结果 | 关键证据 | 判定 |
|---|---|---|---|
| Stage-16 algorithm smoke | `DVR_STAGE16_ALGORITHM_SMOKE_PASSED` | `dvr_nested_smoke.cc` 单元级控制/flatten 检查通过 | 通过 |
| LBD/VTT | baseline/full committed 均为 `996495`；matches `35`，fallbacks `4311`；generated/issued/completed `8/1/1`，covered `1`，fault `0` | 指令守恒和 LBD 路径成立，但 coverage 远低于 M1 的 80% | 未通过 M1 |
| Branch/alternate | helper decoded/issued/completed `2/2/2`，但 divergent branches、alternate complete hits、alternate uops、alternate targets、resumes 均为 `0` | 当前 `dvr_divergent.riscv` 没有形成 lane predicate split | 未通过 M3 |
| NDM | baseline/nested committed 均为 `1279352`；attempts/inversions/outer-found `1/1/1`；outer invocations `2`；flattened/expected `17948/17948`，failures `0` | 当前 `run_remote_dvr_ndm_e2e.sh` 已从本 worktree 编译并执行，branch inversion、outer scan 与 flatten 守恒已发生 | 控制层通过，M2 未通过 |
| NDM data plane | helpers generated/issued/completed `4694/6/6`；replay attempts `6`，但 replay targets `0` | helper request 和 response 生命周期已经进入数据面，但 replay 没有产出 dependent target | 未通过 M2 |

结果目录：

```text
/home/lynnhoo/dvr-repro/results/dvr-lbd-vtt
/home/lynnhoo/dvr-repro/results/dvr-next-helper-regression/20260807T120758-2729514/branch
/home/lynnhoo/dvr-repro/results/dvr-ndm-e2e-script-current-20260807/run
```

脚本路径错误也已确认：`run_remote_dvr_ndm_e2e.sh` 默认指向旧
`/home/lynnhoo/dvr-repro/source/gem5-runahead-dev-pre` worktree，必须显式传入当前
`ROOT`、`GEM5` 和 benchmark；`run_remote_dvr_alternate_path.sh` 默认把 benchmark root
设为 `$ROOT/benchmarks`，而当前专项 ELF 位于仓库顶层 `benchmarks/`。在修正脚本默认路径
并让 NDM generated request 进入 helper LQ/data port 前，禁止继续运行 Figure 8、GAP5 或
论文性能消融并把结果称为 DVR reproduction。

## Alternate-path 当前判断

`dvr_divergent.riscv` 的 alternate continuation 已能生成 dependent target、覆盖 demand
并恢复 reconvergence；真实 BFS 也已跨过 cached-hit 到 alternate uop/target 的 admission
门槛。当前剩余问题是 BFS target 的时序覆盖，以及 Camel 没有在实际输入中产生可观察的
alternate complete path。后续应针对这两个 workload 调整输入或增加专项 workload，而不是
把 zero hit 误判为执行链失败。

## 并行 P0 期间可继续做的非冲突修改

1. alternate-path 专项 workload 和独立检查脚本；
2. 单 lane ready、partial chunk、same-PC batching 和 reconvergence 守恒检查；
3. 论文原配置锚定脚本、CSV provenance 和 committed-instruction 守恒检查；
4. cache quality 结果解析、workload 归档和 debug 文档；
5. GAP/BFS/Camel 的不改变微结构的回归和差异分析。

## 已解决过程与验证记录

### P0 独立 helper 子线程（已合入主 worktree并完成 smoke 验证）

`dvr-p0-helper` 分支的 `7658292` 和 `ee19b8f` 已完成以下子线程内部结构：

- helper 私有 VRAT physical register bank、free-list 和每 replay uop 的 physical
  source/destination mapping；它与主线程的 architectural state 隔离，符合 transient
  in-order subthread 的所有权边界；
- 16-copy VIR 状态：每 copy 有 active/issued/executed/dead-source mask，64-bit 元素为
  16 copies x 8 lanes，最多 128 lanes；
- helper-owned `DVRDynUop` 生命周期和有限 VIR；uop 在共享 FU 完成前保持 issued，完成
  后才 retire；
- 16-entry helper LQ：请求在共享 O3 data port 成功发送后分配 entry，response 回收 entry，
  capacity、translation fault 与 wakeup 均可统计；
- 独立 8-entry timing frontend：`NeedFetch -> FetchPending -> Fetched`，只有 RISC-V
  decoder 返回 `StaticInst` 后才提供 ready credit；
- helper 与主线程共享 FU pool、MMU、data port、cache/DRAM，但不进入主 O3 的 IQ/ROB。

这解决了旧文档中“没有 helper-owned physical/16-copy/LQ/8-entry frontend 状态”的
缺口；主 worktree 已能编译并在 Camel/BFS/LBD 回归中产生 helper DynUop 生命周期统计。
这仍不等于论文 NDM 已 bit-exact；helper 不进入主线程 ROB/commit，fetch/decode 仲裁也
仍是显式 helper frontend 的近似模型。

### Helper fetch/decode

当前 helper cache miss 会从 live SE address space 读取 16-bit 或 32-bit RISC-V instruction，
通过 `Decoder::decodeRaw()` 解码，并写入 helper-owned decoded-uop cache；fetch fault、
decode fallback 和 cache hit 都有独立统计。已有验证中 `dvr_divergent.riscv` 和
`dvr_ndm.riscv` 的 live fetch、decode、fault 和 metadata fallback 均为可解释值，fallback
为零。

这关闭了“完全只使用 captured StaticInstPtr”的功能缺口。随后增加的
`dvr_icache_port` 通过 `Request::INST_FETCH`、MMU execute translation、timing
request/response 和 retry 完成独立缓存访问；miss 不再占用 FU，lane 会在 response 后
继续。仍保留论文级独立 8-entry front-end 与更细粒度 arbitration 的差异。

一次 `dvr_divergent.riscv` 验证得到：

```text
dvrHelperInstructionFetches       190  (fetch attempts; 9 unique timing requests)
dvrHelperInstructionFetchFaults     0
dvrHelperInstructionsDecoded       9
dvrHelperInstructionTimingRequests 9
dvrHelperInstructionTimingResponses 9
dvrHelperInstructionTimingRetries  8
```

该结果证明 helper PC 的真实指令字节经过 timing hierarchy 并收到响应，而不是只从
Discovery 保存的 `StaticInstPtr` 回放；timing miss 期间也没有产生错误的 metadata
fallback。

### RPT innermost stride

`discoverySeen`、`repeatedDuringDiscovery` 和重复内层 stride handoff 已实现，NDM workload
已观察到 branch inversion、outer discovery 和 invocation collection。仍需短 outer/inner
microbenchmark 对齐每 generation 一次 handoff 和 bound pairing。

### Load width 与 ISA 语义

`LB/LH/LW/LWU/LD` 使用实际 `loadBytes` 构造请求，并在 source response 写入 helper VRAT
前执行 RV64 sign/zero extension。`LBU/LHU` 已进入当前构建，后续只需补充专项语义回归。

### Branch divergence 与 persistent reconvergence

已接入 lane PC、active mask、per-lane branch target、same-PC grouping 和 bounded
reconvergence stack。branch group 在更新 lane PC 前汇总 taken/fall-through mask；FLR
后有控制流时继续执行，外部 target 只回收到已知 boundary。

历史 branch 回归曾得到 `dvrDivergentBranches=202`、`dvrReconvergences=679`、stack
overflow `0`，helper DynUop decoded/issued/completed 为 `4416/4416/4416`。

### Alternate-path 执行链

原问题是旧统计只在建立 deferred frame 时计数，并且 provisional FLR boundary 被排除
在 cached suffix 外，导致 alternate path 只有地址计算而没有 dependent load。修复后，
真实 alternate uop issue、terminal load、dependent target 和 reconvergence resume 都
接入 persistent VIR；exact key 未命中时，只允许相同 branch、target、address-space 的
已完成 entry 做有限 reconvergence fallback。

专项脚本：`/home/lynnhoo/dvr-repro/source/gem5-dvr-next/scripts/run_remote_dvr_alternate_path.sh`。
当前 P0 修改后的 `dvr_divergent.riscv` 结果：complete hits `10`，alternate uops `796`，
dependent targets `128`，demand covered `128`，reconvergence resumes `468`，stack
overflows `0`；persistent trace alternate lane-uops `784`，单 lane `136`，partial chunk
`198`，最大 same-PC group `8`。

总 alternate uop 统计比 persistent trace 多出的 `12` 个来自旧 evaluator/continuation
入口，脚本已将两个统计口径分开报告。

### Alternate-path multi-workload 回归

脚本已扩展为支持 `BENCHES=a,b,c`，并把每个 workload 的 baseline/full committed
守恒、缓存命中、实际 alternate uop、dependent target、demand coverage、persistent
trace 和 helper 生命周期写入同一个 `alternate_path.csv`。这次使用当前 `gem5.opt`
运行 `dvr_divergent.riscv`、BFS 和 Camel，结果为：

| workload | committed 守恒 | complete hits | alternate uops | dependent targets | demand covered | 结论 |
|---|---:|---:|---:|---:|---:|---|
| `dvr_divergent` | 323629 = 323629 | 10 | 796 | 128 | 128 | strict pass |
| BFS | 695614 = 695614 | 7 | 0 | 0 | 0 | `cache_hit_only`，未进入 continuation |
| Camel | 517590 = 517590 | 6 | 0 | 0 | 0 | `cache_hit_only`，未进入 continuation |

输出目录：
`/home/lynnhoo/dvr-repro/results/dvr-next-alternate-path-multi/20260805T225513-909327`。
该回归确认问题不是 committed 路径被破坏，而是 BFS/Camel 的 cached alternate hit 到
实际 lane continuation 之间仍有缺口；因此该项继续保留在前面的未解决列表。

### Camel pipeline

Camel 回归曾得到 baseline/full ticks `208039500/201195500`，speedup `1.034017x`，
committed instructions `910847/910847`，DynUop `205098/205098/205098`，active-mask
failures `0`，source/dependent requests `184120/8475`，translation faults `0`。Camel
没有主线程 SIMD 指令，不能单独验证 SIMD FU contention。

### NDM regression

NDM 已验证 branch inversion、outer invocation collection、flatten 守恒和 variable
inner-lane batch。最近结果为 attempts `65`、branch inversions `65`、outer invocations
`130`、flatten batches `3709`、flattened/expected lanes `457411/457411`，nested helper
generated/issued/completed `457411/406430/406430`。这证明 execution-driven NDM data plane
已接通，但不改变上面 P0 的论文级限制。
