# DVR Debug Status

前半部分只保留当前未解决项；已解决内容、验证过程和历史结果统一放在末尾。

## 当前未解决

### 2026-08-06：资源仲裁、VRAT 与专项回归状态

1. **Helper memory/LSQ arbitration 已收紧**：helper request 现在只有在 (a) 主线程
   本周期未用尽 LSU budget、(b) 16-entry helper LQ 未满、(c) 主 LSQ 对该 thread 仍有
   free load entry 时才能使用同一个 LSQ data port；timing response 返回时释放 helper-LQ
   reservation。helper load 仍不创建可 commit 的 `DynInst`，这是为了避免 transient
   prefetch 错误进入 ROB/commit/squash 语义。

2. **VRAT 的正确边界**：当前 `DVRHelperVectorRegisterFile` 是 helper-owned、有限的
   physical bank，存放每 lane 的 transient 值；它不应直接从 `UnifiedFreeList` 借主线程
   scalar physical registers。后者没有 vector-lane storage，且会把 transient helper
   生命周期耦合进 rename/ROB reclaim。共享资源模型应覆盖 FU、LSQ data port、MMU、cache
   和 DRAM；helper register bank 保持独立，符合论文的 DVR 子线程所有权。

3. **VTT/LBD 专项 gate**：已实现 VTT dispatch undo、显式 `slt/sltu + beq/bne` LCR/SBB、
   RISC-V `blt/bge/bltu/bgeu` 虚拟 LCR，以及 `min(128, max_lanes)` loop-bound fallback。
   下次端到端回归必须分别覆盖：显式 LCR、fused LCR、mispredict squash、无 bound fallback、
   divergent path、短 inner-loop NDM。现有 `run_remote_dvr_helper_regression.sh` 已覆盖
   divergent path 与 NDM；其余四项需要独立 microbenchmark gate 后才可标为验证完成。

4. **VIR 16-copy state**：helper VIR 现在固定为 16 个 copy slot，每 slot 管理一个
   8-lane 512-bit copy 的 active mask、PC/uop、issued、executed 与 dead-source 状态。
   同一 copy 在其前一个 helper DynUop 完成前不可重发；不同 copy 仍可在共享 FU 可用时
   独立 issue。该项已通过 `cpu.o` 编译，仍需在有 RISC-V toolchain 的节点运行
   `run_remote_dvr_helper_regression.sh` 验证 16-copy occupancy、分歧与 replay 守恒。

### P0：论文级 helper 微结构

1. **P0 helper 实现尚待合入主 worktree 并做端到端回归**：`dvr-p0-helper` 的
   `7658292`、`ee19b8f` 已实现 helper 私有 VRAT physical bank/free-list、16-copy
   VIR mask/state、16-entry helper LQ、8-entry timing frontend 和 helper DynUop。
   这些是论文所需的独立 in-order subthread 状态，不应改为主 O3 的第二条 ROB 线程。
   但当前主 worktree 的 `cpu.cc/.hh/pre.cc/.hh` 有并行未提交修改，必须先做冲突审查、
   合并并重新验证，不能在主分支提前宣称已生效。

2. **NDM 仍是 execution-driven data-plane model**：branch inversion、IR/ILR/LCR、
   outer invocation collection 和 128-lane flatten 已有，但还不是论文级 bit-exact NDM
   helper pipeline。

3. **共享资源仲裁仍是近似模型**：helper 已使用共享 FU pool、O3 data port、MMU 和
   cache hierarchy，但 fetch/decode 竞争仍以 residual-width 近似；需要在合入后做
   main-priority、round-robin、unlimited/constrained FU 的敏感性验证。

### P1：算法和 workload 覆盖

1. loop-bound detector 仍偏向简单 induction loop；复杂 compare、复杂 induction
   expression、中途进入、early exit 和一般 LCR/SBB 组合仍可能 fallback。

2. `LBU/LHU` 已进入构建，不再作为缺口；但更多 arithmetic、word operation、indirect
   control-flow、break/early-exit 和 path live-out 组合仍需专项测试。

3. BFS/Camel 的 cross-discovery alternate coverage 仍未完成：最新 multi-workload 回归中
   BFS/Camel 虽然分别有 `complete_hits=7/6`，但 `alternate_uops=0`、
   `alternate_targets=0`、`demand_covered=0`。缓存命中尚未进入实际 continuation，仍需
   修复命中后的 lane admission/replay 链，并用 BFS/Camel 重新证明 dependent coverage。

### P2：资源模型和论文复现

1. helper fetch/decode 带宽、主线程竞争、vector FU latency/throughput 和 LSQ contention
   仍需做 unlimited-FU、constrained-FU、主线程优先和 round-robin 敏感性实验。

2. 论文 workload、ROI、cache/MSHR/DRAM 配置尚未完全对齐 Sniper 原配置；需要先做
   NAS-IS、BFS/BC、PageRank 和短 inner-loop microbenchmark 的原配置锚定，再迁移到统一平台。

3. cache quality 仍需绑定完整的 L1 lookup/fill/eviction/victim/invalidate 事件，区分
   useful、late、evicted、pollution 和 demand coverage。

## Alternate-path 当前判断

当前 unresolved 的边界是：`dvr_divergent.riscv` 的 alternate continuation 已经能够生成
dependent target，但 BFS/Camel 只有 cached complete hit，尚未产生 alternate continuation
uop。因此不能把缓存命中统计当成 cross-discovery coverage 已完成。

## 并行 P0 期间可继续做的非冲突修改

1. alternate-path 专项 workload 和独立检查脚本；
2. 单 lane ready、partial chunk、same-PC batching 和 reconvergence 守恒检查；
3. 论文原配置锚定脚本、CSV provenance 和 committed-instruction 守恒检查；
4. cache quality 结果解析、workload 归档和 debug 文档；
5. GAP/BFS/Camel 的不改变微结构的回归和差异分析。

## 已解决过程与验证记录

### P0 独立 helper 子线程（待合入主 worktree）

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

这解决的是旧文档中“没有 helper-owned physical/16-copy/LQ/8-entry frontend 状态”的
缺口；不等于主 worktree 已验证完成，也不等于论文 NDM 已 bit-exact。

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
