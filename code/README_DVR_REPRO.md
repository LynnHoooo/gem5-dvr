# gem5 RISC-V DVR 复现代码导览

> 更新时间：2026-08-03
>
> 项目定位：这是一个 **RISC-V ISA-adapted DVR（Decoupled Vector
> Runahead）机制原型**。它复现论文的核心思想并把 x86/AVX-512 后端映射为
> 128 个逻辑 lane，但不是论文 x86/Sniper 绝对性能数字的逐项复刻。

## 1. 从哪里开始看

本地主要源码根目录：

```text
code/gem5-runahead-dev-pre/
```

建议按以下顺序阅读：

1. `src/cpu/o3/pre.hh`：DVR 各微结构的类和数据结构。
2. `src/cpu/o3/pre.cc`：Stride、Discovery、VTT、Loop Bound、Recorder、
   VRAT、VIR、mask/reconvergence 的算法实现。
3. `src/cpu/o3/dvr_predicate.hh/.cc`：由真实 source response 构造逐 lane
   path mask。
4. `src/cpu/o3/dvr_quality.hh/.cc`：严格质量指标的事件驱动 tracker。
5. `src/cpu/o3/iew.cc` 中的 `IEW::dispatchInsts()` 和 `src/cpu/o3/cpu.cc`
   中的 `CPU::observeDVRDispatch()`：在 O3 dispatch 阶段观察 RPT/stride
   candidate、启动 Discovery，并推进 VTT/FLR 观测。
6. `src/cpu/o3/cpu.cc` 中的 `CPU::instDone()`：在 commit 阶段完成
   Discovery 生命周期，用已记录的 committed slice 生成 helper/replay。
7. `src/cpu/o3/cpu.cc` 中的 `launchDVRStridePrefetches()`、
   `serviceDVRPrefetchQueue()`、`completeDVRPrefetch()`：128-lane helper 和
   dependent prefetch 的真实 cache timing 路径。
8. `src/cpu/o3/lsq_unit.cc`：主线程 load 在 LSQ 发射时报告真实地址，
   更新 RPT/quality 观测。
9. `src/cpu/o3/lsq.cc`：截获并消费 DVR helper 的 cache response。
10. `configs/dvr/table1_se.py`：论文 Table 1 风格的 gem5 配置和 DVR 参数。

如果只想看本次新增代码，可在源码根目录执行：

```bash
rg -n "DVR|dvr" \
  src/cpu/o3/BaseO3CPU.py \
  src/cpu/o3/pre.hh src/cpu/o3/pre.cc \
  src/cpu/o3/cpu.hh src/cpu/o3/cpu.cc \
  src/cpu/o3/lsq.cc src/cpu/o3/lsq_unit.cc \
  configs/dvr/table1_se.py
```

## 2. 文件结构

```text
prefetch/
├── code/
│   ├── README_DVR_REPRO.md              # 本文档
│   └── gem5-runahead-dev-pre/
│       ├── src/cpu/o3/
│       │   ├── BaseO3CPU.py             # DVR 参数
│       │   ├── pre.hh / pre.cc           # DVR 微结构主体
│       │   ├── dvr_nested.hh / .cc       # 两层 Nested 提交生命周期控制器
│       │   ├── dvr_predicate.hh / .cc    # 实际逐 lane predicate mask
│       │   ├── dvr_quality.hh / .cc      # 严格质量事件 tracker
│       │   ├── cpu.hh / cpu.cc           # 状态、统计、commit 与 helper
│       │   ├── lsq_unit.cc               # RPT load observation
│       │   └── lsq.cc                    # helper response 路径
│       └── configs/dvr/table1_se.py      # Table 1 风格配置
├── benchmarks/
│   ├── dvr_stride.c                     # stride/discovery 微基准
│   ├── dvr_dependent.c                  # 两级依赖访存微基准
│   └── dvr_divergent.c                  # 数据依赖分支微基准
├── scripts/
│   ├── build_remote_gem5_dvr.sh
│   ├── run_remote_dvr_stage1_smoke.sh
│   ├── run_remote_dvr_stage3_smoke.sh
│   ├── run_remote_dvr_stage4_smoke.sh
│   ├── run_remote_dvr_stage5_smoke.sh
│   ├── run_remote_dvr_stage6_smoke.sh
│   ├── run_remote_dvr_stage7_smoke.sh
│   ├── run_remote_dvr_stage8_smoke.sh
│   ├── run_remote_dvr_stage9_compare.sh
│   ├── run_remote_dvr_stage10_smoke.sh
│   ├── run_remote_dvr_stage11_control_flow.sh
│   ├── run_remote_dvr_stage12_quality.sh
│   ├── run_remote_dvr_stage14_ndm_control.sh
│   ├── run_remote_dvr_stage15_resource_smoke.sh
│   ├── run_remote_dvr_stage16_algorithm_smoke.sh
│   ├── run_remote_dvr_stage17_quality_workload.sh
│   └── run_remote_dvr_regression.sh     # QUICK/full 一键回归
└── docs/
    ├── 02_reproduction_status.md
    ├── 04_gem5_dvr_implementation.md
    ├── 05_gap_experiment_plan.md
    ├── 06_nested_dvr_design.md
    └── 07_dvr_quality_metrics.md
```

服务器对应目录：

```text
/home/lynnhoo/dvr-repro/source/gem5-runahead-dev-pre
/home/lynnhoo/dvr-repro/scripts
/home/lynnhoo/dvr-repro/results
```

## 3. 具体增加了什么

### 3.1 参数

文件：`src/cpu/o3/BaseO3CPU.py`

```text
enableDVR              开关
dvrRPTEntries=32       RPT entry 数
dvrMaxLanes=128        最大逻辑 lane 数
dvrHelperMaxUops=200   helper 终止上限
dvrDiscoveryMaxInsts=512
```

### 3.2 Stride Detector 与 Discovery

文件：`src/cpu/o3/pre.hh`、`pre.cc`

- `DVRStrideDetector`：32-entry、LRU replacement、signed stride、2-bit
  confidence。
- `DVRDiscoveryController`：当前主路径为 `Idle → Discovering`。
- O3 dispatch 看到训练好的 stride load 后立即启动 Discovery；不等待该
  dynamic load commit。
- dispatch 看到下一次相同 trigger PC 时记录 stop boundary；commit 再按顺序
  完成 Discovery、统计并生成 helper。
- 最多记录 512 条 committed instruction。
- `Abandoned` 路径用于释放由 speculative/squashed load 产生的 armed trigger。

`Armed` 状态仍保留在控制器中，用于兼容旧的 commit-side 生命周期和放弃路径，
但当前 dispatch 主路径不再通过 `Armed` 延迟启动。

NDM 控制器现在额外保存论文所说的控制状态：inner backward branch 的 IR、
induction/bound register 的 ILR/LCR、branch inversion 标志、出口/重汇合 PC 和
剩余迭代数；O3 squash 会回滚尚未提交的 Discovery trigger。

### 3.3 VTT、FLR 和 Loop Bound

- `DVRVectorTaintTracker`：RISC-V x0–x31 的 32-bit VTT。
- tainted source 向 destination 传播 taint。
- 地址源 tainted 的 load 更新 FLR。
- `DVRLoopBoundDetector`：识别包围 trigger-to-FLR 的 backward branch。
- 两次寄存器快照推断 bound、increment、remaining iterations 和 active lanes。

论文的 16-bit VTT 对应 x86 的 16 个架构整数寄存器；本原型扩展到 32 bit，
是对 RISC-V 32 个整数寄存器的 ISA 适配。

### 3.4 Recorder、VRAT 和 VIR

- `DVRInstructionRecorder`：最多保存 256 条 replay metadata uop；8 条是论文中
  front-end buffer 的 decoded-uop refill window，不是整个 trigger-to-FLR template
  的容量上限。
- `DVRVectorRenameTable`：32 个 RISC-V architectural integer registers、
  8 个 16-lane chunk、128 个逻辑 vector physical ID。
- `DVRVectorInstructionRegister`：两个 64-bit active-mask word。
- 按 16-lane chunk 记录 issue/execute。
- 8-entry reconvergence stack；每个 lane 还保存独立的 front-end window、PC 和
  continuation 状态。
- 200-helper-uop termination budget。

这里的 VRAT/VIR 已有真实状态和运行统计。VIR 的 recorder 分支现在保存实际的
branch target 和 committed reconvergence boundary；helper 前端也按
`fetch → decode → ready → issue → drain` 推进。仍未覆盖任意 RISC-V 指令，
真实 dependent address 执行对 unsupported 链仍保留显式 affine fallback。

### 3.5 真实 cache timing 路径

文件：`src/cpu/o3/cpu.cc`、`lsq.cc`

```text
discovery complete
  → 生成最多 128 个未来 trigger 地址
  → DTLB translateAtomic
  → SoftPFReq timing packet
  → L1D/下层 cache
  → source response 返回 8-byte value
  → 选择学习到的 FLR relation
  → 生成 dependent SoftPFReq
```

这些请求进入 gem5 timing memory system，不是只增加统计计数。

### 3.6 模块到代码位置映射

下表给出每个 DVR 模块的主要实现文件、运行时入口和对应的验证入口。除特别
说明外，路径均相对于 `code/gem5-runahead-dev-pre/`。

| 模块 | 主要代码位置 | 关键运行时入口 | 验证入口 |
|---|---|---|---|
| 参数与配置 | `src/cpu/o3/BaseO3CPU.py`；`configs/dvr/table1_se.py` | `enableDVR`、`dvrEnableDependentPrefetch`、`--dvr-mode` | Stage 2；Figure 8 |
| RPT / Stride Detector | `src/cpu/o3/pre.hh/.cc` | `DVRStrideDetector::observe()`（真实 load 地址）；`observeDispatch()`（dispatch 侧 candidate） | Stage 1、3 |
| O3 dispatch 接入 | `src/cpu/o3/iew.cc`；`src/cpu/o3/cpu.cc` | `IEW::dispatchInsts()` → `CPU::observeDVRDispatch()` | Stage 1、3、4 |
| Discovery | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `arm()`、`observeDispatch()`、`observeCommit()`；`CPU::instDone()` | Stage 3 |
| VTT / FLR | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `DVRVectorTaintTracker::observe()`、`classify()`；dispatch 侧 taint/dependent 记录 | Stage 4 |
| Loop-Bound Detector | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `begin()`、`updateFinalLoad()`、`observe()`、`infer()`；RV64 branch compare decode 与 signed bound arithmetic | Stage 5、6 |
| Instruction Recorder | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `DVRInstructionRecorder::begin()`、`record()`；`instDone()` 保存 committed slice | Stage 8、10 |
| VRAT / VIR | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `DVRVectorRenameTable::build()`；`DVRVectorInstructionRegister::execute()`；真实 Discovery register snapshot 初始化 lane | Stage 10、11 |
| Source prefetch | `src/cpu/o3/cpu.cc` | `launchDVRStridePrefetches()`、`serviceDVRPrefetchQueue()` | Stage 7 |
| Dependent replay | `src/cpu/o3/cpu.cc` | `replayDVRSource()`、`completeDVRPrefetch()`；`lsq.cc` 完成 response 回调 | Stage 8 |
| Predicate / reconvergence | `src/cpu/o3/dvr_predicate.hh/.cc`；`cpu.cc` | `DVRLanePredicateTracker`；`retireDVRPredicateLane()` | Stage 11、12 |
| Lane-PC unsupported path audit | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `executeLanePC()`；`unsupportedControlFlow`；`dvrVIRUnsupportedControlFlow` | Stage 11 |
| Lane termination classes | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | normal completion、branch early-exit、external target、unsupported semantic counters | Stage 11 |
| VIR-only control fallback | `src/cpu/o3/cpu.cc`；`cpu.hh` | `CPU::instDone()` 在 VIR 仅因控制流/语义不支持而拒绝时保留 source helper；timeout、stack overflow、recorder overflow 仍禁止启动 | Stage 7、11 |
| Source-value VIR continuation | `src/cpu/o3/pre.hh/.cc`；`cpu.cc` | `initializeSourceContinuation()` 为整个 helper 保存 lane PC/register/window 状态；`resumeSourceLanes()` 按当前 PC 合并 ready lanes；scalar replay 使用独立 FLR prefix | Stage 11 |
| Nested / Multiple | `src/cpu/o3/dvr_nested.hh/.cc`；`cpu.cc` | `DVRNestedController`、`DVRNestedDiscoveryMode`；`completeDVRNestedContext()`、`launchDVRNestedPrefetches()` | Stage 13、14 |
| Quality metrics | `src/cpu/o3/dvr_quality.hh/.cc`；cache/LSQ 接线处 | `DVRQualityTracker::issued()`、`completed()`、`demandLookup()`、`cacheFill()` | Stage 12、15 |
| Cache / LSQ timing | `src/cpu/o3/lsq_unit.cc`；`src/cpu/o3/lsq.cc`；`cpu.cc` | load address observation、helper packet response、prefetch queue service | Stage 7、8、9 |
| Figure 8 消融 | `scripts/run_remote_dvr_figure8.sh` | `run_case()`；VR / Offload / Discovery / Multiple 映射 | Figure 8 |
| NDM algorithm state | `src/cpu/o3/dvr_nested.hh/.cc`；`src/cpu/o3/dvr_nested_smoke.cc` | IR/ILR/LCR、branch inversion、outer invocation gate | Stage 16 |

Stage 7/14/15/17 脚本默认从脚本位置解析仓库根目录，使用
`code/gem5-runahead-dev-pre` 的最新构建；如果该 checkout 没有 benchmark，会自动回退
到相邻的 `gem5-runahead-dev-pre/benchmarks`。也可以通过 `ROOT`、`BENCH`、`GEM5`
覆盖默认路径。

NDM 和 helper 前端的专项统计也可由以下入口检查：

- `dvrNDMIRCaptures`、`dvrNDMILRCaptures`、`dvrNDMLCRCaptures`、
  `dvrNDMOuterInvocations`：NDM 控制状态是否真实建立。
- `dvrHelperFetchCycles`、`dvrHelperDecodeCycles`：helper 是否实际经过独立
  fetch/decode 阶段。
- `dvrHelperComputeCycles` 以及 `dvrHelperALUOps`、`dvrHelperShiftOps`、
  `dvrHelperMultiplyOps`、`dvrHelperLSUOps`：captured helper program 的 FU
  profile 和 compute 前段占用；这是资源模型增量，不等价于完整共享 issue queue。
- `scripts/run_remote_dvr_stage14_ndm_control.sh`：NDM 控制状态 smoke。
- `scripts/run_remote_dvr_stage15_resource_smoke.sh`：helper 前端和共享资源
  smoke。
- `scripts/run_remote_dvr_stage16_algorithm_smoke.sh`：不依赖 gem5 workload，
  直接验证 NDM 控制状态和两 invocation flatten gate。

## 4. 当前验证状态

### 4.1 LeAP BFS/Camel 初步 workload 验证

针对 `/home/lynnhoo/gem-test/gem5-leap/leap-bench`，新增脚本
`scripts/run_remote_dvr_leap_bfs_camel_smoke.sh`。它只覆盖两个初步验证目标：

```text
gap/src/bfs.cc       -> leap_bfs_qemu-linux
hpc/camel/camel.c    -> leap_camel_qemu-linux
```

脚本默认消费已经构建的 RV64 Linux ELF，并分别运行 Baseline 和 Nested DVR，
检查 BFS 的 `BFS Tree has` 及 verifier 输出、Camel 的 `Result` 输出，以及
stride candidates、Discovery starts、helper/source request 和 demand address
统计。若本机有工具链，也可以用 `BUILD=1 LINUX_CC=... LINUX_CXX=...` 直接从
两个源码构建单进程 ELF。脚本不使用 qemu-linux 的 fork wrapper，因为 gem5
SE 运行该 wrapper 会在 `fork()` 处退出；脚本也不会把只有源码而没有成功执行
的情况记为通过。

示例（默认 smoke 输入为 BFS scale 6、Camel `MAX_KEY=1024`）：

```bash
BENCH_ROOT=/home/lynnhoo/gem-test/gem5-leap/leap-bench \
BUILD=1 \
LINUX_CC=/path/to/riscv64-unknown-linux-gnu-gcc \
LINUX_CXX=/path/to/riscv64-unknown-linux-gnu-g++ \
scripts/run_remote_dvr_leap_bfs_camel_smoke.sh
```

当前环境若未提供 RISC-V Linux 交叉编译器或两个 ELF，脚本会明确报告缺失项；
这属于构建前置条件，不应伪造为 workload 级验证结果。规模可通过
`BFS_SCALE`、`CAMEL_MAX_KEY` 调整；也可以直接用 `BFS` 和 `CAMEL` 指定已构建的
单进程 ELF。

在 2026-08-03 的本地临时副本 smoke 中，BFS scale 6 的 verifier 为 `PASS`，
Nested DVR 产生了 47,652 个 stride candidate、3,062 次 Discovery start 和
600 个 source request；Camel `MAX_KEY=1024` 产生了 1,355 个 candidate 和
262 次 Discovery start。gem5-leap 的 qemu-linux wrapper 本身会调用 `fork()`，
在 gem5 SE 中会因 syscall 不支持而退出，所以脚本构建的是直接 benchmark main，
这组结果是流程验证，不是论文规模的性能结论。

| Stage | 内容 | 当前状态 | 已获得的证据 |
|---|---|---|---|
| 1 | RPT stride detection | 通过 | 174317 loads，173525 candidates |
| 2 | Table 1 baseline config | 通过 | 自动配置 smoke test |
| 3 | Discovery/timeout | 通过 | 5080 completions，5222 forced timeouts |
| 4 | VTT/FLR | 当前树通过 | 12401 tainted、2396 dependent loads/FLR |
| 5 | Loop Bound | 当前树通过 | 2396 bounds / discoveries |
| 6 | Lane inference | 当前树通过 | 2396 matches，665909 total active lanes |
| 7 | 128-lane cache injection | source-only 通过 | `--dvr-no-dependent-prefetch`：generated=18123，issued/completed=10666，faults=0 |
| 8 | 真实逐 lane dependent replay | 需单独复核 | 代码已有 dispatch sequence tracking、地址有效性保护和 replay 统计；不要用 Stage 7 结果替代 Stage 8 证据 |
| 9 | Baseline vs DVR | 当前树通过 | demand L1D miss 降低 11.56% |
| 10 | recorder/8-uop front-end window/VRAT/VIR | 当前树通过 | recorder capacity 与 8-uop refill window 解耦；真实 replay 守恒断言通过 |
| 11 | actual-value predicate/reconvergence/timeout | 通过（控制流分类与 source continuation） | `divergent=2`、`reconvergences=2`、`dependent=256`、`source_value_vir_executions=328`、`source_value_external_lanes=164`；仍不是完整并行 SIMT |
| 12 | predicate/quality 独立严格 smoke | 当前树通过 | actual-value mask 与质量计数器 `-Werror` smoke |
| 14 | NDM 控制与 timeout | 通过 | dispatch Discovery、IR/ILR/LCR、至少两个 outer invocation、timeout/fallback |
| 15 | helper 前端与资源竞争 | 通过 | fetch/decode/issue、主线程优先资源统计 |
| 16 | NDM IR/ILR/LCR 与 outer invocation gate | 通过 | branch inversion、两个 outer invocation 后 Vectorizing、timeout fallback |
| 17 | L1D workload 级 quality 事件 | 通过 | demand=294915，issued=9910，completed=8354，fills=1462，timely=1266，late=1036，coverage=0.008353，timeliness=0.549957 |

Stage 9 的当前稳定结果：

```text
baseline_cycles=2462727
dvr_cycles=2462523
speedup=1.000083
baseline_demand_l1d_misses=250819
dvr_demand_l1d_misses=221823
miss_reduction=11.56%
```

该微基准证明了 cache miss coverage，但不代表已经复现论文的 2.4× 绝对结果。

此前 Stage 8/10 测试曾获得真实 trigger-to-FLR replay 的硬断言结果：

```text
replay_supported=7188 replay_unsupported=0
replay_unstable_inputs=0
replay_attempts=97419 replay_targets=97419 replay_fallbacks=0
```

该历史测试中的 RVC 链为 `load → C.SLLI → C.ADD → C.LD`。模板在 FLR 截断，
source response 的真实 64-bit load value 被写入每个 lane 的寄存器快照，再依次
执行地址生成 uop。`targets == attempts` 且 `fallbacks == 0`，因此这里的
dependent target 已不再由仿射 fast path 产生。

Stage 11 在加入主线程优先节流和 predicate 判别位修复后的最新完整结果：

```text
starts=8520 completions=8519 abandons=4567 programs=3027
relations=2 distinct_predicate_paths=2
divergent=3019 reconvergences=604 predicate_generation_abandons=2415
predicate_selections=238232 predicate_misses=100
dependent_prefetch_generated=238232
forced_timeouts=1965 forced_generated=0
```

正常组的 3019 个实际 divergence 全部以 reconvergence 或显式 abandon 终止；
lane mask 来自真实 source value，不再由 lane 奇偶生成。强制
`dvrHelperMaxUops=1` 的第二组也证明 timeout 后不会发出 helper prefetch。

Stage 8 当前可严格证明的 bandwidth：

```text
dvrQualityIssuedBytes=880040
dvrQualityCompletedBytes=880040
```

accuracy、coverage、timeliness 和 pollution 需要 L1 tag lookup/fill/victim
回调；在接线前旧的 possibly-useful/late 仍只标记为 proxy。

## 5. 还缺什么

按重要程度排列：

1. 将已验证的逐 lane evaluator 扩展到更多 RV64/RVC 整数、比较、load-value 和地址生成 opcode；
   仿射逻辑仅保留为 unsupported 链的显式 fallback。
2. 将当前按 source response 重建的单 lane VIR continuation，提升为持久化的
   多 lane helper context：为每个未完成 lane 保存 PC、寄存器、active/deferred
   mask 和 reconvergence stack，并让 source response 直接恢复对应 lane，而不是
   重新建立一个单 lane evaluator。
3. 将已验证的实际 value-predicate 路径选择进一步扩展到任意 branch opcode，
   并让 VIR 使用独立 lane PC 执行 branch target/fall-through，而不仅是
   recorder 内的有限路径。
4. 将当前已加入 fetch/decode/issue 状态的 helper 继续扩展为执行端口级资源
   竞争模型。
5. 将已接通的严格质量 tracker 扩展到更多 workload，并统一论文的 accuracy、
   coverage、timeliness 和 pollution 报告口径。
6. 缩小版 GAP workload。
7. Baseline、PRE、Offload/Discovery、Nested DVR 消融。
8. 最终实验报告；Stage 7 source-only、Stage 14 NDM control 和 Stage 15
   helper resource smoke 已通过，Stage 8 dependent replay 和
   后续 workload 级结果仍需按当前二进制重新归档。

Nested 专用验收：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage13_nested.sh
```

服务器最新证据为 `contexts=440, programs=2, vrat=80, vir=80,
generated/issued/completed=256/251/251`，并且真实 child replay 为
`attempts=210, targets=210, fallbacks=0, nested_dependent=210`。child 独立持有 taint、recorder、
loop-bound、register snapshots、VRAT、VIR 和 replay template；relation predictor
与物理 helper queue 仍由 root/child 共享。

此前一次完整回归的摘要为：

```text
DVR_REGRESSION_PASSED quick=0
summary=/home/lynnhoo/dvr-repro/results/dvr-regression-logs/20260730T151859Z.summary
```

随后在服务器单独验证了 Stage 14 的缓存质量事件接口：

```text
DVR_CACHE_QUALITY_EVENT_SMOKE_PASSED
```
这只验证事件载荷、origin 三态和多 victim 传递；尚未宣称 workload 级
accuracy/coverage/timeliness/pollution 已接入。

因此当前不能写“faithfully reproduce the DVR paper”。推荐表述：

> We implement an ISA-adapted prototype of DVR on a RISC-V
> microarchitecture.

## 6. 如何在服务器编译

先进入用户指定的 Nix 环境：

```bash
ssh pre
cd ~/buckyball
nix develop
cd ~/dvr-repro/source/gem5-runahead-dev-pre
```

当前可工作的构建命令：

```bash
PYTHON_ROOT=/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15 \
ZLIB_DEV=/nix/store/h7ik0g1xxayy0z8h27zbvrgmac63irgs-zlib-1.3.2-dev \
ZLIB_LIB=/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2 \
CCFLAGS_EXTRA=-I/nix/store/h7ik0g1xxayy0z8h27zbvrgmac63irgs-zlib-1.3.2-dev/include \
LINKFLAGS_EXTRA=-L/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2/lib \
PYTHON_CONFIG=/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15/bin/python3.11-config \
/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15/bin/python3.11 \
  -m SCons build/RISCV/gem5.opt -j32
```

`protoc`、HDF5、tcmalloc 和 GCC 15 template constructor 信息目前是 warning；
最终出现 `scons: done building targets.` 才算构建成功。

## 7. 如何运行和查看结果

先做快速回归（跳过构建、Stage 5–9 和 Stage 11 的耗时重复仿真）：

```bash
QUICK=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

完整回归（默认先编译，再串联 Stage 1–12）：

```bash
~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

若二进制已是最新，仅跳过编译但保留完整测试：

```bash
SKIP_BUILD=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

每一步的独立日志和总摘要写入
`~/dvr-repro/results/dvr-regression-logs/`。任一步失败时脚本立即返回非零。
也可单独执行：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage8_smoke.sh
~/dvr-repro/scripts/run_remote_dvr_stage9_compare.sh
~/dvr-repro/scripts/run_remote_dvr_stage10_smoke.sh
~/dvr-repro/scripts/run_remote_dvr_stage11_control_flow.sh
```

查看关键统计：

```bash
grep -E 'system.cpu.dvr|system.cpu.numCycles|dcache.ReadReq.misses' \
  ~/dvr-repro/results/dvr-stage9-dvr/stats.txt
```

结果目录以脚本中的 `OUT`/`RESULT_ROOT` 为准，默认位于：

```text
~/dvr-repro/results/
```

## 8. 阅读代码时最重要的边界

- `SoftPFReq` 是真实 cache timing request，但不会修改架构状态。
- helper 当前没有复用 O3 scalar execution units 执行任意 RISC-V uop。
- Stage 8 的仿射 relation 能正确覆盖测试中的两级地址生成，不代表支持任意依赖链。
- reconvergence stack 已实现结构和终止语义；实际 value-predicate 驱动的
  两路径微基准已经通过，但尚未覆盖任意 branch opcode 或复杂多分支程序。
- RISC-V 实验可用于机制探索和同 ISA 对比，不能直接宣称复现论文 x86 speedup。

更详细的设计说明见：

```text
docs/04_gem5_dvr_implementation.md
docs/02_reproduction_status.md
docs/05_gap_experiment_plan.md
docs/06_nested_dvr_design.md
```

## 9. 复现缺口审计与后续方案

### 9.0 2026-08-01 状态更新

本节以下内容基于当前本地开发分支和服务器日志更新。必须区分“代码已提交”和
“服务器已重新编译并验证”：

| 项目 | 当前状态 | 证据边界 |
| --- | --- | --- |
| VIR 私有向量寄存器 | 代码已加入 | 尚未在服务器重新编译后的 Stage 10 验证 |
| 逐 lane uop evaluator | 代码已加入，支持已有有限语义 | 尚未重新生成 workload 级结果 |
| helper thread 生命周期 | 代码已加入 | 尚未用新二进制完成完整回归 |
| branch target / deferred mask / reconvergence metadata | 代码已加入 | 尚未完成任意 branch opcode 验收 |
| 服务器最新 Stage 1–8 | 通过 | 使用服务器当时可执行的构建结果 |
| 服务器最新 Stage 9 | 不可作为新代码结论 | 该次回归使用 `SKIP_BUILD=1`，运行的是旧二进制 |
| 服务器最近一次完整构建 | 失败 | 当时 `pre.hh` 与 `pre.cc` 版本不同步，出现 `vectorRegs`/`laneValue` 编译错误 |

因此，完成新代码验证的正确顺序是：

```text
重新编译
  → Stage 10–14（VIR、predicate、Nested、NDM）
  → Stage 8–9（dependent replay 和性能对比）
  → 必要时完整 Stage 1–14 回归
```

在上述验证完成前，不应把新增 VIR、helper 或 reconvergence 代码写成“已通过
服务器验证”。

### 9.1 总体判断

当前代码不是空壳，而是一个已经通过专用微基准验证的 gem5/RISC-V DVR
机制原型：Stage 1–12 的主路径已有完整回归证据，Nested DVR 也有 Stage 13
专项证据；helper 请求确实进入 gem5 timing cache，source load 返回的真实数据
能够驱动逐 lane dependent-address replay。

但是，目前还不能称为“完整复现 DVR 论文”。更准确的定位是：

> We implement an ISA-adapted prototype of DVR on a RISC-V
> microarchitecture.

当前差距主要分为三层：

1. **机制语义差距**：helper 还不是论文描述的完整有序向量子线程。
2. **Nested DVR 差距**：当前两层 child context 尚不等价于论文的 NDM 算法。
3. **实验差距**：尚缺论文 workload、输入、ROI、消融和严格质量指标。

### 9.2 已经完成且可信的部分

- 32-entry RPT、commit-ordered Discovery、32-register VTT/FLR、loop-bound 和
  lane-count 推断均已接入主执行路径。
- 已实现最多 128 lane、256-entry replay metadata、8-uop front-end refill window、
  VRAT/VIR 状态和 200-uop timeout。
- helper source/dependent 请求经过 DTLB、O3 LSQ data port、cache backpressure、
  cache hierarchy 和 response completion，不是只增加统计计数。
- 当前微基准的 `load → C.SLLI → C.ADD → C.LD` 链已经由真实 source value
  驱动 replay，Stage 8/10 得到 `attempts == targets` 且 `fallbacks == 0`。
- actual-value predicate、两路径 mask、reconvergence 和 timeout 已有专用验收。
- cache 侧已经提供 DemandLookup、Fill、Remove、victim 和 provenance 事件出口，
  严格质量统计不需要重写，只缺最后的 L1D listener/binding。

### 9.3 P0：必须优先解决的缺口

#### 9.3.1 一键完整回归尚未包含 Stage 13（已完成）

`run_remote_dvr_regression.sh` 当前只执行到 Stage 12，然后直接输出
`DVR_REGRESSION_PASSED`。因此现有 full regression 不能自动证明同一源码版本的
Nested Stage 13 也通过。

应在非 QUICK 回归中加入：

```bash
run_step stage13-nested \
    "$SCRIPT_DIR/run_remote_dvr_stage13_nested.sh"
```

QUICK 模式可以跳过，但摘要中必须明确记录 `[SKIP] stage13-nested`。

已实现：`run_remote_dvr_regression.sh` 在非 QUICK 模式下执行 `stage13-nested`，
QUICK 模式记录 `[SKIP] stage13-nested`。最新一次完整回归 Stage 1–13 全部通过：

```text
DVR_REGRESSION_PASSED quick=0
summary=/home/lynnhoo/dvr-repro/results/dvr-regression-logs/20260730T134006Z.summary
```

#### 9.3.2 Nested Discovery Mode 数据面状态

论文 NDM 的完整语义包括：

1. inner loop 可用 lane 少于 64 时触发 NDM；
2. 改变 inner backward branch 的方向；
3. 从 inner loop 后继续执行；
4. 保存 Increment Register（IR）和 Inner Load Register（ILR）；
5. 查找外层 striding load；
6. 先向量化多个 outer-loop invocation；
7. 为每个 invocation 收集 inner-loop 起始地址和 bound；
8. 展平为最多 128 个 inner lanes；
9. 再从 inner stride 启动普通 DVR。

当前 next helper 分支已经把这些控制和数据面接通：完成的短 inner-loop discovery
在 `<64` lanes 时进入 NDM，保存 IR/ILR/LCR，反转 inner backward branch，接受
真实 committed outer stride，并把多个独立 outer invocation 的 inner bound 和
起始地址展平为最多 128 个 scalar-equivalent lanes。NDM 的请求仍经过同一 helper
生命周期和真实 LSQ/cache 路径。

验收不应只看 NDM 控制计数，还必须检查以下守恒关系：

```text
nested_batches > 0
outer_instances >= 2 * nested_batches
flattened_lanes == expected_flattened_lanes
flatten_invariant_failures == 0
nested_helpers_generated >= flattened_lanes
```

`scripts/run_remote_dvr_helper_regression.sh` 会在
`dvr_divergent.riscv`、`dvr_nested.riscv` 和
`dvr_nested_variable.riscv` 上执行这组检查。当前 next binary 的两个 Nested
结果为：普通 nested `6292` batches、`12584` outer instances、`765240`
flattened lanes；variable nested `1518` batches、`3036` outer instances、
`55734` flattened lanes 和 `1210` 个 variable-lane batches。两组的 flatten
invariant failures 都为 0。

这证明了论文 4.3 节的 NDM branch-inversion 到 outer-times-inner flatten
数据面，而不是只证明控制器状态机。它仍然不是 Sniper/x86 的 bit-exact
复现，helper-owned DynInst 也仍不进入主线程 ROB/IQ/commit。

#### 9.3.3 helper 执行资源与前端边界

论文要求 helper 与主线程共享执行单元，并且只有在相同端口没有 main-thread
ready instruction 时才能发射。当前 next helper 已按以下边界建模：

- helper 自有 decoded-uop cache，Discovery 保存的 `StaticInstPtr` 只在 helper
  context 内按 PC 缓存，不进入主线程 fetch queue；
- 主线程阶段先运行，helper fetch/decode 只获得剩余的前端宽度，并统计
  `dvrHelperFetchBlockedByMain` 和 `dvrHelperDecodeBlockedByMain`；
- 同 PC ready lanes 合并为 512-bit chunk，分别请求 `SimdAlu`、`SimdShift` 或
  `SimdMult`，再由 `IEW::tryIssueDVRHelperFU()` 使用原生 FU pool 的 latency
  和占用；
- dependent load 等待前序 helper 地址 uop 完成，然后进入真实 LSQ、DTLB、cache
  和 MSHR；
- 每 lane 持有 PC、8-entry reconvergence stack、helper-uop timeout 和私有 VRAT
  状态；
- helper 仍不产生主线程 DynInst，也不进入主线程 rename/IQ/ROB/commit，这是
  论文独立 in-order subthread 的边界，而不是第二个 SMT O3 线程。

因此它现在是 paper-faithful execution-driven helper timing model，而不是
完整 Sniper 内部实现。性能实验仍需分别报告 constrained vector FU 和
unlimited vector FU，避免把资源模型变化误认为算法收益。

#### 9.3.4 逐 lane evaluator 的 opcode 覆盖过窄

当前主要支持 `ADD/ADDI/SLLI/ANDI/load-address` 和测试使用的
`C.ADD/C.SLLI/C.LD`。不支持的 uop 会使 replay invalid，并回退到 learned
仿射地址 relation。

真实图算法中还常见：

- `SUB/SUBW`、`ADDW/ADDIW`；
- `AND/OR/XOR`；
- `SLL/SRL/SRA` 及 immediate/word variants；
- `LB/LBU/LH/LHU/LW/LWU/LD`；
- sign/zero extension；
- `MUL`；
- 更多 RVC 地址生成形式。

如果真实 workload 大量回退，实验主要验证的是 relation predictor，而不是 DVR
动态指令链向量化。因此必须报告 supported-uop ratio、replayable-chain ratio、
affine-fallback ratio 和 unstable-input ratio。

#### 9.3.5 divergence 尚不是完整 SIMT 控制流执行

当前 actual-value predicate 已覆盖两路径微基准，但尚缺：

- `BEQ/BNE/BLT/BGE/BLTU/BGEU` 的通用逐 lane evaluation；
- branch operand 直接来自 lane register state；
- 按真实 target/fall-through PC 分组；
- 多层分支、break/early exit；
- divergent path 内继续执行后续 dependent load；
- 8-entry stack overflow 的完整回退策略。

正式验收应满足：

```text
divergences == reconvergences
             + explicit_abandons
             + stack_overflows
             + timeouts
```

#### 9.3.6 严格质量指标尚未绑定到指定 L1D（已完成）

`DVRQualityTracker` 和 cache 事件出口已经存在，但配置中尚未创建
`ProbeListenerObject` 并只绑定 `system.cpu.dcache` 的 `"DVR Quality"` probe。

当前能够严格报告 issued/completed requests 和 bytes；尚不能用于最终报告的指标有：

- issued/fill accuracy；
- coverage；
- timeliness；
- average lead time；
- pollution evictions/misses；
- unused DVR evictions。

listener 必须只监听指定 L1D，不能同时监听 L2/L3，否则会重复记账。

已实现：`src/cpu/o3/probe/dvr_quality_probe.{hh,cc}` 与 `DVRQualityProbe.py`
提供 `ProbeListenerObject`，通过 `configs/dvr/table1_se.py --dvr-quality-probe`
以 `manager=l1d` 只绑定 L1D，shadow 几何由 L1D 的 size/assoc/line 推导。

`dvr_dependent.riscv` 实测：

```text
fills=17419  usefulTimely=13313  unusedEvictions=4084
pollutionEvictions=7853  coveredMisses=12660  shadowDemandMisses=151563
fillAccuracy=0.764280  coverage=0.083530  averageLeadTime=175713.58
```

`demandAccesses` / `actualDemandMisses` 与 gem5 自带的
`system.cpu.dcache.demandAccesses/demandMisses` 完全一致，可作为记账正确性交叉验证。

仍未解决：`usefulLate` 只能由 CPU 侧 issue 流观测，cache-only listener 看不到，
因此 `timeliness` 目前恒为 1.0，`issuedAccuracy` 仍需与 CPU 侧 `dvrQualityIssued`
合并计算。这两项在 README 与 stat 描述中均已显式标注，不得直接引用。

#### 9.3.7 论文 workload 和 ROI 尚未准备

论文使用 13 个 benchmark：

- GAP：bc、bfs、cc、pr、sssp；
- Camel、Graph500、HJ2、HJ8、Kangaroo、NAS-CG、NAS-IS、RandomAccess；
- Kron、LiveJournal、Orkut、Twitter、Urand 等固定图输入；
- 跳过初始化后模拟代表性的 500M instructions。

当前没有完整的 RISC-V binaries、图输入、ROI、输入 checksum 和输出语义验证。
Stage 9 的 `1.000083×` 只是微基准功能对照，不能外推到论文的 `2.4×`。

### 9.4 P1：进入论文趋势实验前应补齐的内容

#### 9.4.1 消融基线

当前自动比较只有 Baseline 与 Full DVR。论文趋势复现至少需要：

| 配置 | Stride trigger | Offload | Discovery | Divergence | Nested |
|---|---:|---:|---:|---:|---:|
| Baseline | — | — | — | — | — |
| PRE | — | — | — | — | — |
| VR-like | 是 | ROB/full-window trigger | 否 | 基础 | 否 |
| Offload | 是 | 是 | 否 | 是 | 否 |
| Discovery DVR | 是 | 是 | 是 | 是 | 否 |
| Full DVR | 是 | 是 | 是 | 是 | 是 |

IMP 和 Oracle 可作为第二阶段补充。所有配置必须使用相同二进制、输入、ROI 和
Table-1 风格配置。

#### 9.4.2 Table 1 映射边界

当前 Table 1 是合理近似，但不是逐周期复制：

- gem5 O3 未精确表达论文 15-stage front end；
- `SimpleMemory` 不等同于 Sniper request-based contention；
- stride prefetcher 的 16 streams 可映射，但 degree=4 是推定值；
- RISC-V compiler/codegen、RVC、branch semantics 与 x86/AVX-512 不同。

因此最终目标应区分“RISC-V 机制/趋势复现”和“论文 x86/Sniper 绝对数字复刻”。

#### 9.4.3 innermost stride selection

论文在 Discovery 中维护每个 RPT entry 的 seen bit；若另一 stride 在当前 trigger
重现前出现两次，应切换到更内层 stride，并重置 VTT/FLR。当前单 active discovery
和 nested-candidate 逻辑尚不能替代这一通用算法，应增加 outer/inner stride 专项
微基准和 trigger-switch 统计。

#### 9.4.4 loop-bound fallback 语义

论文在 loop-bound 匹配失败时使用 128 lanes。当前安全实现会记录 fallback，但
抑制 helper，避免未知边界越界。建议增加明确模式：

- `safe`：推断失败不发 helper；
- `paper`：推断失败使用 128 lanes。

最终实验需报告两种模式影响，不能把 safe 模式直接描述成论文原行为。

#### 9.4.5 硬件开销重新核算

论文的 1139 bytes 对应 x86 原设计，不能直接用于当前 RISC-V 实现。应重新列出：

| 结构 | entries | bits/entry | 总字节 | 论文原结构 | 模拟器专用 |
|---|---:|---:|---:|---|---|

重点区分真正硬件状态与模拟器 bookkeeping，包括 32-bit VTT、32-register VRAT、
child context、predicate/replay metadata、relation predictor 和 quality shadow state。

#### 9.4.6 MLP 和资源统计

论文的核心论据是 DVR 显著增加 outstanding memory requests。当前还应增加：

- average/peak L1 MSHR occupancy；
- demand 与 DVR 分项 MSHR occupancy；
- memory queue occupancy；
- cache/data-port utilization；
- helper 被主线程抑制的周期比例；
- execution-port utilization；
- DRAM bandwidth utilization。

这些指标用于解释 miss reduction、speedup 和 timeliness，而不是只报告最终 cycles。

### 9.5 分级复现目标

#### Level A：机制正确性复现

证明 Discovery、dependent replay、lane count、divergence、timeout、严格 NDM、
quality accounting 和 Stage 1–13 regression 在微基准上可重复。

#### Level B：同 ISA 趋势复现

在 RISC-V/gem5 上验证 Offload、Discovery 和 Nested 的贡献趋势，报告 performance、
MLP、accuracy、coverage、timeliness、bandwidth 和 pollution。

#### Level C：论文结果近似复现

尽可能对应论文 Figure 7–12，但明确 simulator、ISA、compiler 和 memory model
差异。成功标准应是机制排序和趋势可解释，而不是强制得到论文的 `2.4×`。

### 9.6 推荐实施阶段

#### 阶段 0：冻结证据并修复回归入口

1. 将 Stage 13 加入 full regression。
2. 每次保存 git SHA、dirty status、构建命令、compiler version、benchmark hash、
   `config.ini`、`stats.txt` 和 stdout/stderr。
3. 本地与服务器使用相同 Git commit。
4. 回归摘要明确显示 Stage 1–13 的 PASS/SKIP/FAIL。

#### 阶段 1：接通严格 L1D 质量指标

1. 实现 L1D `ProbeListenerObject`。
2. 转发 DemandLookup、Fill 和 Remove。
3. 导出 accuracy、coverage、timeliness、lead time、pollution 和 unused eviction。
4. 用 timely、late、unused/polluting、redundant 四组微基准验证。

#### 阶段 2：扩展 replay evaluator

第一批支持 SUB、word arithmetic、逻辑运算、全套 shift、不同 load width、常见
RVC 和 MUL。每个 semantic 必须有单元测试，不支持的操作必须显式计数。

微基准验收：

```text
replay_attempts == replay_targets
replay_fallbacks == 0
unsupported_uops == 0
```

真实 workload 的建议门槛：replayable chains ≥90%，fallback source responses ≤5%。

#### 阶段 3：完成通用控制流 replay

支持六类 RISC-V 条件分支、真实逐 lane operand、target PC/mask 分组、多层路径、
path 内 dependent load、break 和 stack overflow。

#### 阶段 4：实现论文语义的 NDM

建议状态机：

```text
Normal Discovery
  ├─ lanes >= 64 → Normal DVR
  └─ lanes < 64  → NDM

NDM
  ├─ invert inner backward branch
  ├─ scan outside inner loop
  ├─ find and vectorize outer striding load
  ├─ collect inner start/bound per outer invocation
  ├─ flatten to at most 128 inner lanes
  └─ launch ordinary DVR from inner stride
```

新增统计建议：

- `dvrNDMAttempts`
- `dvrNDMSuccesses`
- `dvrNDMFallbacks`
- `dvrNDMTimeouts`
- `dvrNDMOuterLanes`
- `dvrNDMInnerLanesCollected`
- `dvrNDMInnerLanesDiscarded`
- `dvrNDMHelpersGenerated`

微基准至少覆盖 inner bound 4、inner bound 20 和 NDM 200-uop timeout/fallback。

#### 阶段 5：建模 helper 执行资源竞争

先实现按 operation class 的每周期资源 token，并保持 main-thread priority；随后如有
必要，再把 helper uop 接入 gem5 FU/issue scheduling。分别用 ALU、shift、load-port
和 bandwidth 饱和微基准验证。

#### 阶段 6：准备 workload

1. 先跑 GAP BFS scale-10 smoke，只用于流程验证。
2. 再固定能超过 LLC、且仿真时间可接受的小图，运行 bc/bfs/cc/pr/sssp。
3. 随后加入 RandomAccess、NAS-IS、NAS-CG、Graph500、HashJoin 等。
4. 每项记录 source commit、compiler flags、ELF、input checksum、ROI 和输出 checksum。

#### 阶段 7：执行消融矩阵

每个 workload 至少运行 Baseline、PRE、VR-like、Offload、Discovery DVR 和 Full DVR。
收集 cycles、IPC、cache misses、branch MPKI、MSHR occupancy、helper traffic、严格质量
指标、fallback ratio 和 NDM success ratio。

#### 阶段 8：敏感性实验

至少扫描：

- lanes：32/64/128/256；
- recorder：4/8/16 uops；
- helper timeout：100/200/400；
- NDM threshold：32/64/96；
- ROB：128/192/224/350/512；
- stride degree：1/2/4/8；
- main-thread priority on/off；
- memory latency 和 bandwidth。

### 9.7 推荐执行顺序

不要先直接跑大规模 benchmark。推荐顺序为：

1. Stage 13 加入回归；
2. 严格 L1D quality listener；
3. 统计 workload opcode/fallback coverage；
4. 扩展 evaluator；
5. 通用 branch replay；
6. 真正的 NDM；
7. MLP/resource contention；
8. GAP smoke；
9. GAP 五 workload；
10. 消融；
11. hpc-db；
12. 敏感性和最终报告。

如果 evaluator 大量 fallback、quality 不可测且 NDM 仍不是论文算法，即使提前得到
speedup，也无法判断收益来自 DVR、仿射 predictor，还是 cache traffic 的偶然效应。

### 9.8 最终完成标准

只有同时满足以下条件，才建议写“完成 RISC-V/gem5 DVR 机制与趋势复现”：

- Stage 1–13 一键 full regression；
- 目标 workload 上 evaluator fallback 足够低；
- 六类 RISC-V branch 的逐 lane divergence 验证通过；
- NDM 实现 `<64 lanes → outer invocation collection → flatten to 128`；
- helper 具有可解释的共享执行资源竞争；
- L1D accuracy、coverage、timeliness 和 pollution 可报告；
- 至少完成 GAP 五 workload；
- 完成 Baseline、PRE/VR-like、Offload、Discovery、Full DVR 消融；
- MLP 与 speedup 能相互解释；
- workload、二进制、输入、ROI、配置和结果均有 manifest；
- 文档明确区分 x86/Sniper 论文数字与 RISC-V/gem5 结果。

按上述标准，当前可粗略评估为：

- 微基准机制原型：约 70%–80%；
- 论文算法忠实度：约 45%–55%；
- 论文实验复现度：约 15%–25%。

现阶段最重要的三个下一步是：**真正的 NDM 数据路径、真实 workload 与消融矩阵、执行资源竞争建模**。

## 10. Stage 14：NDM 控制语义第一阶段

在服务器提交已完成严格 L1D quality probe 和 Stage 13 full-regression 接入后，
下一迭代开始实现论文 Nested Discovery Mode 的独立控制状态：

- `dvrNDMThreshold=64`，仅可信 inner lane count 小于阈值时启动；
- `dvrNDMMaxInsts=512`，独立限制 NDM 搜索 outer stride 的提交预算；
- 保存 inner trigger PC（ILR 语义）、loop increment（IR 语义）和 inner lanes；
- outer stride 必须不同于 inner trigger，并通过动态 load sequence 的 commit 过滤；
- commit-budget timeout 后显式回退 ordinary inner DVR；
- 导出 attempts、outer-found、fallback 和 timeout 统计；
- 新增 `dvr_ndm.c` 与 Stage 14 正常/禁用/timeout 三组服务器验收。

运行：

```bash
bash scripts/run_remote_dvr_stage14_ndm_control.sh
```

Stage 14 当前已通过服务器验证，并实现了 branch-direction 控制记录、IR/ILR/LCR
捕获、outer candidate commit 过滤、bounded outer plan 和 timeout/fallback。仍未达到
论文完整 NDM 的部分是：helper 必须自主反转 inner branch 后搜索 outer striding load，
跨多个动态 outer invocation 收集各自 bound，并由该计划统一驱动真实 flatten-to-128；
当前模型仍以已提交 child context 和 event-driven helper 为主。

## 11. Stage 15：资源竞争统计与固定构建环境

Stage 15 已加入回归脚本，统计 `dvrHelperIssueCycles`、`dvrHelperComputeCycles`、
captured ALU/shift/LSU profile、`dvrResourceConflicts`、
`dvrPrefetchesSuppressedMainThread` 和 `dvrPrefetchesIssued`。运行：

```bash
bash scripts/run_remote_dvr_stage15_resource_smoke.sh
```

通过条件是模拟周期、helper issue 周期和 DVR 请求均大于零，且 helper issue 周期不
超过总周期。2026-08-01 的服务器验证已经通过：

```text
DVR_STAGE15_RESOURCE_PASSED cycles=2463975 helper_fetch=6091 helper_decode=6252 helper_compute=8367 alu_ops=38742 shift_ops=19371 lsu_ops=38742 helper_issue_cycles=9910 conflicts=14555 main_thread_suppressed=13480 dvr_issued=9910
```

服务器构建统一使用 Python 3.11.15 ABI，同时保留 `nix develop` 注入的
`PYTHONPATH`，由 Python 3.11 加载纯 Python 的 SCons 4.10.1。不要清除
`PYTHONPATH`，也不要改用 Python 3.13；后者会使该 gem5 版本的 `gem5py_m5`
生成器段错误。首次 bootstrap 前清理旧 ABI 生成物：

```bash
cd ~/dvr-repro/source/gem5-runahead-dev-pre
rm -rf build/RISCV
unset PYTHONHOME
export PYTHON_CONFIG=/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15/bin/python3.11-config
export CCFLAGS_EXTRA=-I/nix/store/h7ik0g1xxayy0z8h27zbvrgmac63irgs-zlib-1.3.2-dev/include
export LINKFLAGS_EXTRA=-L/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2/lib
export LIBRARY_PATH=/nix/store/61a1nwx3w6rqyaisj5rn1sal1981apm7-zlib-1.3.2/lib
/nix/store/28wlfb25i3q4wq06ap0n9gb04qkjdjyn-python3-3.11.15/bin/python3.11 -m SCons build/RISCV/gem5.opt -j32
```

该组合已完成一次从空 `build/RISCV` 开始的完整构建并通过 Stage 15。若未来出现
Python ABI 变化，必须重新清理 `build/RISCV`，不能复用其他 ABI 的生成物。

### 11.2 residual resource model 验证结果

提交 `1cea464d` 在 helper 发射前统计主线程实际执行的 issue、ALU 和 LSU 使用量，
并增加 issue/ALU/LSU 三类 residual-budget conflict。服务器增量编译和 Stage 15
通过：

```text
DVR_STAGE15_RESOURCE_PASSED cycles=2462727 helper_issue_cycles=9
conflicts=0 issue_conflicts=0 alu_conflicts=0 lsu_conflicts=0
main_issue=1656847 main_thread_suppressed=1 dvr_issued=9
```

同一二进制的 Stage 9 性能门槛没有通过：Baseline 和 DVR 均为 2462727 cycles，
data misses 分别为 250819 和 250821。当前 helper 只发出 9 个请求，因此不能把该
微基准写成性能收益。严格 L1D listener 的独立 tracker/event/predicate smoke 均通过；
在 `dvr_dependent.riscv` workload 上观察到 9 issued/9 completed，但没有 DVR fill，
所以 fill accuracy 和 timeliness 为 `nan`、coverage 为 0。该结果表明下一步必须先
提高有效 helper traffic 并把 CPU issue/completion stream 与 L1D listener 合并，
再运行正式消融，不能用此前的代理质量计数器替代。

### 11.3 helper budget 修复与 Stage 9/quality 复验

提交 `92d6e82b` 将 VIR 的 200-uop helper 预算从“每个 scalar lane 操作”改为
“每个已发射 vector chunk uop”计费；提交 `188d4fea` 将 branch target、fall-through
和 reconvergence PC 放回真正的 recorder `Uop`，而不是 stride-table entry。服务器
复验结果：

```text
DVR_STAGE9_COMPARE_PASSED baseline_cycles=2462727 dvr_cycles=2462631
speedup=1.000039 baseline_misses=250819 dvr_misses=215054
miss_reduction_pct=14.26
```

helper timeout/suppression 均从 3874 降为 0，generated/issued 分别为
300184/107725；资源模型观察到 48871 次冲突，其中 issue 9856、LSU 39015。

真实 L1D listener 的 workload 结果：

```text
fills=18698 usefulTimely=14561 coveredMisses=13973 unusedEvictions=4123
pollutionEvictions=8651 pollutionMisses=0 fillAccuracy=0.778746
coverage=0.092193 averageLeadTime=179511.95 timeliness=1.000000
```

这里的 `timeliness=1.0` 是接通 issue/completion 生命周期之前的旧结果，已由
第 13 节的新验证取代；不能继续把它作为最终及时性数据引用。

## 12. 六组消融实验（服务器实测）

提交 `c2dbe36e` 增加显式 `--dvr-mode`，避免用互不等价的参数冒充消融机制：

- `vr`：只对稳定 stride 做 128-lane vector runahead，不进行依赖发现；
- `offload`：动态 Discovery + 单 lane helper；
- `discovery`：只运行 Discovery/VIR 分析，不发 helper；
- `full`：Discovery + 128-lane helper，禁用 Nested controller；
- `nested`：Full DVR + Nested controller/NDM。

服务器命令：

```bash
bash ~/dvr-repro/scripts/run_remote_dvr_ablation.sh
```

结果路径：`~/dvr-repro/results/dvr-ablation/summary.csv`。在
`dvr_dependent.riscv` 上的实测结果如下：

| Mode | Cycles | IPC | Demand misses | Helper issued | Conflicts | Fill accuracy | Coverage | Pollution evictions |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 2462727 | 0.545546 | 250819 | 0 | 0 | n/a | 0 | 0 |
| PRE/VR-like | 2471851 | 0.543533 | 148911 | 413896 | 260565 | 0.438777 | 0.139764 | 10754 |
| Offload | 2462727 | 0.545546 | 250819 | 1911 | 0 | n/a | 0 | 0 |
| Discovery | 2462727 | 0.545546 | 250819 | 0 | 0 | n/a | 0 | 0 |
| Full DVR | 2462631 | 0.545568 | 215054 | 107725 | 48871 | 0.778746 | 0.092193 | 8651 |
| Nested DVR | 2462631 | 0.545568 | 215054 | 107725 | 48871 | 0.778746 | 0.092193 | 8651 |

该微基准中 Full DVR 的 demand misses 下降 14.26%，cycles 只下降 96（约
0.0039%）。VR-like 虽进一步降低 misses，但过量 helper traffic 和资源冲突使 cycles
增加约 0.37%。Full 与 Nested 相同不是 Nested 已证明无效，而是该 dependent
workload 没有形成可利用的 outer × inner 数据面；Nested 效果必须在 NDM/nested
workload 和后续 GAP workload 上单独报告。

## 13. Quality 生命周期与 Nested 数据面验证

提交 `bef3f354` 为 L1D quality listener 增加 DVR request 的 Issue/Complete 事件，
并按 cache line 完成 outstanding request；因此 demand miss 可以区分 fill 后使用的
`usefulTimely` 和请求尚未完成时发生的 `usefulLate`。服务器在
`dvr_dependent.riscv` 上的结果为：

```text
issued=107725 completed=105962 fills=18698
usefulTimely=13973 usefulLate=36790
fillAccuracy=0.747299 coverage=0.092193 timeliness=0.275260
```

`completed < issued` 表示模拟结束时仍可能存在未观察到 hit/fill completion 的请求，
不能把两者差值解释成有用或无用请求。关键验证点是 `usefulLate` 已不再恒为零，且
timeliness 由真实 issue/fill/demand 时序计算。

提交 `cdd26cd8` 修复单一 speculative nested-candidate 槽被年轻 load 覆盖的问题，
并删除通过 `trigger + stride × i × 16` 合成四个 outer base 的近似。Nested context
现在只保存实际提交的首次 trigger 地址和关闭该 child 的 recurrence 地址。运行：

```bash
bash ~/dvr-repro/scripts/run_remote_dvr_nested_data_smoke.sh
```

2026-08-01 固定 Python 3.11 ABI 二进制的服务器实测：

```text
ndm batches=1 outer_instances=2 inner_lanes=15 flattened_lanes=30 generated=30 issued=60
nested batches=4 outer_instances=8 inner_lanes=512 flattened_lanes=512 generated=512 issued=145
DVR_NESTED_DATA_PASSED
```

每个 batch 的 flattened lane 数受 128-lane 上限约束；`nested` 的累计值为 4 个
batch 各 2 个真实 invocation、每批 128 个 inner lanes。该 smoke 证明真实动态地址
已进入 `outer_instances × inner_lanes` flatten 路径，但还不是 GAP benchmark 的性能
结论，也尚未证明每个 outer invocation 可拥有不同的独立 inner bound。

### 13.1 逐 outer invocation 独立 inner bound

提交 `3436d330` 修正了上一版仍然共享 inner bound 的问题：关闭 child 的 recurrence
不再被提前计作一个“已完成”的 invocation；每次 child completion 只提交已经完整
观察的 `base + innerLanes`，跨 completion 收集至少两个实例后才生成 Nested batch。
`dvrNestedVariableLaneBatches` 只在同一 batch 中存在不同 lane count 时递增。

新增 `dvr_nested_variable.c`，让动态 outer invocation 在 16/32 次 inner loop 间
交替。服务器使用 2026-08-01 17:41 编译的 Python 3.11 ABI 二进制运行：

```text
ndm batches=0 outer_instances=0 inner_lanes=0 flattened_lanes=0
nested batches=3 outer_instances=6 inner_lanes=768 flattened_lanes=384
variable batches=5 outer_instances=10 inner_lanes=152 flattened_lanes=152
         variable_lane_batches=5 generated=152 issued=218
DVR_NESTED_DATA_PASSED
```

`ndm` 只有一个满足数据面条件的 child completion，因此严格语义下不能组合两个
已完成 invocation；其零 batch 是预期结果，而不是再用未知 bound 制造假阳性。
固定 256-inner workload 的每批总 lane 数超过 128，所以 768 个推断 inner lanes
实际 flatten 为 384。变长 workload 中 5 批全部包含独立且不同的 bound，且未触及
上限时 `flattened_lanes == inner_lanes == 152`。

同一新二进制的非 Nested 回归也通过：

```text
DVR_STAGE15_RESOURCE_PASSED cycles=2462523 helper_issue_cycles=108851
conflicts=47604 issue_conflicts=9352 alu_conflicts=0 lsu_conflicts=38252
main_thread_suppressed=281494 dvr_issued=108851
DVR_CACHE_QUALITY_EVENT_SMOKE_PASSED
DVR_PREDICATE_SMOKE_PASSED
```

至此“每个已完成 outer invocation 独立保存 inner bound 并参与 flatten”已验证；仍未
完成的是 GAP workload 级性能验证，以及任意长、多分支 trigger-to-FLR 路径覆盖。

## 14. GAP BFS scale-10 workload smoke

脚本 `scripts/run_remote_dvr_gap_bfs_smoke.sh` 固定使用官方 GAPBS 提交
`2972aeb2703165bafd921222f4ed7196f542d3a8`，以 GCC 15.2.0、`SERIAL=1` 和
`-std=c++11 -O3 -static -fno-tree-vectorize` 构建 RV64GC BFS。ELF SHA-256：

```text
cc568ed7fb349ca3f927cc639d1df2cfd0aabd15a105f381fe798bec6cd83059
```

新 glibc 在静态启动时调用 `riscv_hwprobe(258)` 和 `rseq(293)`；gem5 22 原表缺少
这两个入口。`se_workload.cc` 现在为二者返回 Linux 标准 `-ENOSYS`，使 glibc 使用
非 hwprobe/rseq fallback，而不是伪报成功。长图 workload 还暴露默认 5-slot
IEW→commit transport buffer 不足；Table-1 配置将 `forwardComSize` 增至 64，但
`wbWidth=5`、issue/commit width 和其余核心资源不变。

DVR helper 原来始终构造 8-byte Packet，未经过架构 load-splitting，跨 cache line
时会触发 L1D 断言。现在 source helper 仅在完整 8 bytes 位于同一 line 时读取，跨线
lane 安全终止；只需要 cache 副作用的 dependent `SoftPFReq` 使用 1-byte Packet。

服务器命令：

```bash
bash ~/dvr-repro/scripts/run_remote_dvr_gap_bfs_smoke.sh
```

输入固定为 `-g 10 -n 1`，输出确认 1024 nodes、10496 undirected edges。结果：

| Mode | Ticks | IPC | L1D demand misses | Helper issued | Conflicts | Nested batches |
|---|---:|---:|---:|---:|---:|---:|
| Baseline | 2741965500 | 0.929941 | 101921 | 0 | 0 | 0 |
| Full DVR | 2730218500 | 0.933944 | 103505 | 274544 | 24749 | 0 |
| Nested DVR | 2730218500 | 0.933944 | 103505 | 274544 | 24749 | 0 |

Full 相对 Baseline 的 ticks 下降约 0.43%，但 demand misses 增加约 1.55%；quality
listener 报告 fill accuracy 1.0、coverage 0.028553、timeliness 0.997672 和 678 次
pollution eviction。图规模很小且未形成 Nested batch，因此这些数据只证明真实 GAP
二进制、输入、统计和 DVR 路径可运行，不能作为性能收益或 Nested 有效性的结论。

证据路径：

```text
~/dvr-repro/results/gap-bfs-s10/manifest.txt
~/dvr-repro/results/gap-bfs-s10/summary.csv
~/dvr-repro/results/gap-bfs-s10/{baseline,full,nested}/stdout.log
```

同一二进制随后通过 Stage15 和变长 Nested 数据面回归；后者仍得到非零
`variable_lane_batches`。下一步是构建 bc/cc/pr/sssp，并选择超过 LLC、但仿真时间
可接受的固定图输入，再执行六模式消融。

### 14.1 GAP 五 workload 的三模式流程验证

同一固定 GAPBS 提交、编译器、`-g 10 -n 1` 输入和新 gem5 二进制完成了
BC/BFS/CC/PR/SSSP 的 Baseline、Full、Nested 共 15 次全程序运行。校验脚本：

```bash
bash ~/dvr-repro/scripts/collect_remote_dvr_gap5_smoke.sh
```

汇总位于 `~/dvr-repro/results/gap5-s10/summary.csv`，并通过
`DVR_GAP5_S10_RESULTS_VALIDATED`。关键结果：

| Workload | Baseline ticks | Full ticks | Nested ticks | Baseline misses | Full misses | Nested batches/outer/lanes |
|---|---:|---:|---:|---:|---:|---:|
| BC | 2886148250 | 2874269250 | 2875078250 | 114667 | 119356 | 57 / 114 / 2702 |
| BFS | 2741965500 | 2730218500 | 2730218500 | 101921 | 103505 | 0 / 0 / 0 |
| CC | 2722113750 | 2709197750 | 2709197750 | 100814 | 102885 | 10 / 20 / 1280 |
| PR | 3122066000 | 3107696000 | 3108382000 | 145919 | 169727 | 4283 / 8566 / 285973 |
| SSSP | 3308999500 | 3248807250 | 3248902250 | 224112 | 230784 | 15 / 30 / 320 |

Full 在五个 scale-10 workload 上的 ticks 均低于 Baseline，幅度约 0.4%–1.9%，但
L1D demand misses 也均增加；这说明当前小图收益不能解释为 miss reduction，可能来自
执行时序变化，必须结合更大图、MLP 和带宽统计再判断。Nested 在 BC/CC/PR/SSSP
形成真实 batch，其中 PR 的 Nested traffic 明显增加且略慢于 Full；BFS 未触发。

这仍是三模式流程验证，不是论文要求的六模式正式消融。下一步必须运行 VR-like、
Offload、Discovery，并把图规模提高到能够超过 LLC，同时控制仿真时间。

## 15. GAP 五 workload 六模式消融

脚本 `scripts/run_remote_dvr_gap5_ablation.sh` 已完成五个 workload × 六个模式共 30
次运行。`vr_like` 目录标签映射到 CLI 的合法参数 `--dvr-mode=vr`；脚本同时检查
每个后台 job 的退出状态和非空 `stats.txt`。结果：

```text
~/dvr-repro/results/gap5-ablation-s10/summary.csv
GAP5_DVR_ABLATION_PASSED
```

下表给出 cycles（单位为 gem5 ticks）；括号内为 helper issued：

| Workload | Baseline | VR-like | Offload | Discovery | Full DVR | Nested DVR |
|---|---:|---:|---:|---:|---:|---:|
| BC | 2886148250 | 2663538250 (9219733) | 2885642250 (9763) | 2886148250 | 2874269250 (346411) | 2875078250 (347649) |
| BFS | 2741965500 | 2517964500 (8713440) | 2741365500 (9071) | 2741965500 | 2730218500 (274544) | 2730218500 (274544) |
| CC | 2722113750 | 2492037750 (8608174) | 2722113750 (6940) | 2722113750 | 2709197750 (292594) | 2709197750 (293077) |
| PR | 3122066000 | 2893802750 (10241887) | 3122249000 (15617) | 3122066000 | 3107696000 (712518) | 3108382000 (810632) |
| SSSP | 3308999500 | 3031403250 (9821398) | 3305743250 (844) | 3308999500 | 3248807250 (494587) | 3248902250 (494863) |

模式行为符合预期：Discovery 与 Baseline 完全相同，因为它只运行发现和 replay
分析而不发 helper；Offload 只产生少量单 lane traffic；Full/Nested 产生稳定的
helper stream；VR-like 的 helper traffic 和资源冲突数量远高于其它配置。

VR-like 的 cycles 明显更低、misses 也更低，但它在 BFS/BC/CC/PR/SSSP 分别发出
约 8.6M–24.6M helper 请求，资源冲突最高超过 1.1M，不能直接当作“更好的 DVR”，
而应视为高流量 PRE 对照。Full/Nested 的收益较小且部分 workload 的 demand misses
增加，必须结合更大图和 MLP/MSHR 统计解释，不能把这些 scale-10 结果外推为论文的
绝对 speedup。

Nested flatten 统计：BC `57/114/2702`、BFS `0/0/0`、CC `10/20/1280`、PR
`4283/8566/285973`、SSSP `15/30/320`（分别为 batches/outer instances/
flattened lanes）。

## 16. P0 evaluator 进展

服务器最新提交上已扩展 `DVRInstructionRecorder::Semantic` 和逐 lane evaluator，
新增 RV64 常见整数数据流：`SUB`、寄存器 `AND/OR/XOR`、寄存器移位
`SLL/SRL/SRA`、`MUL`，以及 `ORI/XORI/SRLI/SRAI`。二元语义现在显式读取第二个
source register；未覆盖指令仍返回 `Unsupported`，不会静默近似。

固定 Python 3.11/Nix 环境重新编译成功，Stage15 资源验证通过：

```text
DVR_STAGE15_RESOURCE_PASSED cycles=2494131 helper_issue_cycles=94199
conflicts=51594 issue_conflicts=10290 alu_conflicts=0 lsu_conflicts=41304
main_issue=2003631 main_thread_suppressed=242974 dvr_issued=94199
```

这完成了 P0 evaluator 的第一批可审计扩展。当前源码已经包含
`ADDW/SUBW`、word shift、`LB/LH/LW/LWU/LD` 的 evaluator/replay 分支；但真实
memory request 的 load width、sign/zero extension、RVC 变体、完整 branch path
evaluator 和严格的论文 NDM branch inversion 仍然需要独立验收。当前 P0 状态应记录为：

- Stage13 一键回归：已完成；
- GAP 五 workload 六模式实验：已完成；
- evaluator 常见整数语义：已扩展并编译/Stage15 验证；
- 完整论文 NDM、branch inversion 和 shared fetch/decode contention：尚未完成；
  普通 DVR 的独立 VIR/VRAT helper 路径已完成初步接入。

## 17. O3 pipeline 接入状态

`38b4aa8` 的 load-width evaluator 已合并到当前分支；随后又完成了 helper
compute 到 gem5 原生 O3 FU pool 的接入。当前路径是：

```text
committed Discovery slice
  -> DVRHelperThread fetch/decode/ready state
  -> IEW::tryIssueDVRHelperFU()
  -> FUPool::getUnit(IntAlu/IntMult)
  -> freeUnitNextCycle()
  -> helper memory uop -> LSQ data port -> cache response
```

对应代码位置：

- `src/cpu/o3/iew.hh/.cc`：`IEW::tryIssueDVRHelperFU()`，调用原生
  `FUPool::getUnit()`、读取 native op latency，并按 O3 生命周期预约 FU 释放；
- `src/cpu/o3/cpu.cc`：`CPU::issueDVRHelperCompute()`，把 captured ALU/shift
  映射到 `IntAluOp`，把 multiply 映射到 `IntMultOp`，并在主线程 issue 后参与
  residual issue arbitration；
- `src/cpu/o3/cpu.cc`：`serviceDVRPrefetchQueue()`，helper memory request
  仍经过 O3 LSQ 的 data port、DTLB、cache timing response 和 completion；
- `scripts/run_remote_dvr_stage15_resource_smoke.sh`：验收 native FU request、
  grant、stall 以及 LSQ/helper 资源统计。

新二进制的 Stage 15 结果为：

```text
fu_requests=120921 fu_grants=89129 fu_stalls=31792
helper_fetch=9394 helper_decode=9641 helper_compute=25288
helper_issue_cycles=10414 dvr_issued=10414
```

因此当前可以称为 **ISA-adapted, execution-driven, in-order vector helper
with private VIR/VRAT and O3 FU/LSQ timing**。边界仍需准确保留：helper 不进入
主线程的 `DynInst`、IQ、ROB 或 commit；这是符合论文独立 in-order subthread
边界的选择。当前 helper 的 front-end 仍从 committed replay template 建立，尚未
完成论文级的 decoded front-end buffer 和主线程 fetch/decode bandwidth arbitration。

### 17.1 M1-M4：独立 helper 指令与私有 VIR/VRAT

当前普通 DVR source-response 路径已不再直接调用旧的 evaluator 生成 dependent
地址，而是经过以下对象和状态：

```text
source response
  -> ReplayLaneContext
  -> private DVRHelperVectorRegisterFile / VRAT
  -> DVRDynUop (Decoded -> Ready -> Issued -> Completed)
  -> finite VIR buffer (8 entries)
  -> shared SimdAlu/SimdShift/SimdMult FU
  -> dependent LSQ/cache request
```

对应实现位于 `src/cpu/o3/cpu.hh/.cc`：

- `DVRHelperVectorRegisterFile`：每个 replay program 私有的架构寄存器到
  helper vector physical register 映射、lane value、valid/ready 状态；不使用主线程
  的物理寄存器文件，也不产生 commit 状态；
- `DVRInstructionRecorder::Uop::staticInst` 和 `DVRDynUop::staticInst`：Discovery
  保存 gem5 ISA 解码对象，helper 使用该对象作为 decoded-uop metadata，而不是重新
  构造一个主线程 `DynInst`；
- `DVRHelperThread::DVRDynUop`：保存 PC、operand、active lane mask、chunk 数量、
  issue/complete tick 和生命周期状态；
- `DVRHelperThread::virBuffer`：有限 8-entry 的 in-order VIR buffer；
- `CPU::issueDVRReplayLanes()`：按相同 PC/uop 和 ready lane 形成最多 512-bit chunk，
  预约原生 FU，并在完成地址计算后提交 dependent request。

Camel 验收结果（vector-chunk helper build）：

```text
dvrHelperDynUopsDecoded       2,212,641
dvrHelperDynUopsIssued        2,212,641
dvrHelperDynUopsCompleted     2,212,641
dvrHelperVRATPrograms            17,251
dvrHelperVRATWrites           3,942,774
dvrVIRContinuationMaxGroupWidth        8
dvrDependentDemandCovered        65,495
dvrDependentDemandLate                 1
```

该路径已经包含显式 helper/main fetch-decode residual arbitration、同 PC
multi-lane batching 和 helper-owned branch PC/reconvergence 状态；分支和 NDM
回归见 9.3.2。仍不能描述为 Sniper 论文实现的 bit-exact reproduction，原因是
gem5 RISC-V 与 Sniper x86 的前端、TLB、缓存填充和带宽细节并不相同。

## 18. DVR 第一阶段原配置锚定与 NDM 端到端验收

当前 table1 配置已对齐论文中最重要的核心/缓存参数：5-wide、350-entry ROB、
32KB L1D、24 L1D MSHR、256KB L2 和 8MB LLC。它仍是 gem5/RISC-V timing model，
不是 Sniper/x86 的逐项等价替代，因此结果应称为机制锚定，而不是论文数值复现。

原配置锚定脚本：

```bash
ROOT=/path/to/gem5-runahead-dev-pre \
BENCH_ROOT=/path/to/benchmarks \
BENCHES=dvr_dependent.riscv,dvr_ndm.riscv \
bash scripts/run_remote_dvr_anchor.sh
```

脚本输出 trigger、discovery completion、loop-bound match、平均 lanes、dependent
traffic、quality、DVR outstanding cache lines 和峰值。当前 dvr_dependent 结果显示
Full DVR 平均 outstanding helper lines 为 5.65、峰值 18，但 loop-bound match
只有 207/4421、coverage 为 0.007865，周期略慢于 baseline；这证明有 decoupled
traffic，却不能称为已复现论文的 2.4x DVR。

NDM 数据面验收脚本：

```bash
bash scripts/run_remote_dvr_ndm_e2e.sh
```

它强制检查 branch inversion、outer invocation、outer x inner flatten、dependent
replay target，以及 helper generated/issued/completed 和逐 batch flatten 守恒。
修复后的专项结果为 attempts=44、branch_inversions=44、outer_instances=2320、
flattened_lanes=14478、replay_targets=3531、helper completed=9649，守恒失败为零。

同一修复版二进制的 GAP scale-10 Nested 运行也全部进入数据面：

| Workload | NDM attempts | Outer invocations | Nested batches | Outer instances | Flattened lanes | Helper completed |
|---|---:|---:|---:|---:|---:|---:|
| BC | 387 | 501 | 240 | 480 | 19859 | 18457 |
| BFS | 40 | 74 | 33 | 66 | 4074 | 3662 |
| CC | 49 | 80 | 34 | 68 | 4351 | 4094 |
| PR | 5747 | 11242 | 5616 | 11232 | 408190 | 376417 |
| SSSP | 56 | 50 | 21 | 42 | 2538 | 2397 |

因此当前应把配置分层称为 DVR-Offload、DVR-Offload+Discovery、DVR-Nested，
而不是直接把所有 Full/Nested 结果标为完整论文 DVR。只有简单间接 workload
出现稳定、可解释的正收益，并且 outstanding misses、coverage、timeliness 和
资源竞争都与收益一致后，才升级命名和论文结论。

### 18.1 Camel Figure 3 架构验收（2026-08-05）

使用当前 `gem5-dvr-next` 构建和
`camel-dvr-trace-c_lw-full/camel.riscv`，验收脚本为：

```bash
BENCH=/home/lynnhoo/dvr-repro/results/camel-dvr-trace-c_lw-full/camel.riscv \
  bash scripts/run_remote_dvr_camel_arch_check.sh
```

脚本不只比较最终结果，还检查 `Result`、`committedInsts`、VRAT/VIR/UOP
守恒和 `DVR_TRACE_DIR` 生成的地址链。最近一次结果：

```text
Result                              33888308
baseline simTicks                  208039500
Full Vector simTicks               201195500
Full Vector speedup                    1.034017x
vectorizer source lanes               184120
vectorizer dependent lanes              8475
helper DynUops decoded/issued/completed 205098/205098/205098
private VRAT programs/writes          2024/368240
VIR active-mask checks/failures       205098/0
maximum same-PC VIR group width             8
dependent requests issued/completed    8475/8475
possibly-useful / late prefetches    11416/175
dependent demand covered / late       8149/3
peak outstanding DVR lines               19
```

这组结果证明 Camel 中确实走过以下普通 DVR 数据路径：

```text
RPT candidate
  -> Discovery + loop bound
  -> source vector lanes
  -> real source ReadReq through LSQ/cache
  -> source value written to private VRAT lane
  -> same-PC ready lanes grouped by VIR
  -> helper DynUop uses native SimdAlu FU reservation
  -> dependent replay target
  -> real dependent SoftPFReq through LSQ/cache
```

`vectorization.csv` 与 `dependency_chain.csv` 还会检查：每个 VIR trace group
不超过 8 个 64-bit elements；每个 dependent target 都有同一 trigger/lane
更早到达的真实 `source_value`；source lane 的 lane 编号和 Camel 8-byte 地址
对齐有效。所有消融模式的程序结果和 committed instructions 保持一致。

该 Camel 程序没有短 inner-loop 的 outer invocation 结构，因此
`nested_flatten_batches=0` 是预期结果，不能用它验收 NDM；NDM 仍由
`run_remote_dvr_helper_regression.sh` 的 Nested microbenchmark 验收。

同时，Camel 主线程没有使用 SIMD 指令，所以 `dvrVectorFUConflictCycles=0`，
且 constrained/unlimited vector-FU 两次结果相同。这不表示资源模型已经验证了
主线程 SIMD 竞争，只表示该 workload 没有提供这条证据。helper uop 使用的是
独立 `DVRDynUop -> VIR -> IEW::tryIssueDVRHelperFU -> FUPool` 路径；它仍不进入
主线程 `DynInst -> rename -> IQ -> ROB -> commit`，因此这里的结论是
“ISA-adapted execution-driven helper”，不是 bit-exact 的 Sniper DVR。

### 18.2 Camel Figure 3 pipeline diagnostic：初学者阅读方法

本节对应脚本
`scripts/run_dvr_camel_pipeline_diagnostic.sh`。它不检查程序退出/清理结构，
只检查普通 DVR data path：RPT、Discovery、VTT/FLR、loop bound、recorder、
VRAT、vectorizer、VIR、helper uop、FU 和 LSQ/cache。运行：

```bash
cd /home/lynnhoo/dvr-repro/source/gem5-dvr-next
BENCH=/home/lynnhoo/dvr-repro/results/camel-dvr-trace-c_lw-full/camel.riscv \
  bash scripts/run_dvr_camel_pipeline_diagnostic.sh
```

脚本会打印一个新的结果目录。最重要的阅读方法是：

```text
数量 > 0                         = 该模块确实被执行
generated == issued == completed  = 请求没有卡在 helper/cache 队列
issued == completed               = 已发出的请求都收到 response
mask failures == 0                = active lane mask 没有和实际 lane 不一致
max group width >= 2              = 至少有一次真正的多 lane 同 PC issue
useful/covered > 0                = 预取至少帮助了主线程的一部分访问
late 较高                         = 预取发出过晚，不能及时隐藏 miss
```

下面按图中的模块解释每一段输出。

| 输出段 | 看什么 | 说明 |
|---|---|---|
| `1. RPT / stride detector` | `loads observed`、`stride candidates` | RPT 先观察主线程 load，再判断哪些 load 的地址呈固定步长。candidate 不能为零，否则后面不会启动 DVR。 |
| `2. Discovery Mode` | `starts`、`completions`、`timeouts` | `starts` 表示触发了 Discovery；`completions` 表示找到了可以结束的 discovery。timeout 不一定是错误，但过多说明探索经常没有在限制内完成。 |
| `3. VTT / taint / FLR` | `tainted instructions`、`dependent loads`、`discoveries with FLR` | taint 表示“这条指令的输入来自 trigger load”；dependent load 是最终间接地址 load；FLR 是 Discovery 找到的最终 load register。三者都为正，说明依赖链确实被追踪。 |
| `4. Loop-bound detector` | `bounds`、`matches`、`lane-count samples` | bound 是循环剩余次数，lane count 是要向量化的迭代数量。`matches` 表示两个动态寄存器快照支持这个推断；`fallbacks` 表示推断失败而使用最大 lane 数，应作为准确率风险观察。 |
| `5. Recorder / replay` | `recorded metadata uops`、`programs built`、`unsupported`、`overflows` | recorder 保存的是 replay 所需的指令 metadata。`unsupported=0` 最好；`overflows>0` 表示有 discovery 超过 recorder 可保存的模板，需要单独修复，不能把这一轮称为所有链都被完整记录。 |
| `6. VRAT` | `programs`、`lane register writes` | VRAT 是 helper 私有的向量寄存器表。每个 replay program 应创建一个 VRAT，source response 应写入 lane register。 |
| `7. Vectorizer` | `source lanes`、`dependent lanes` 和 CSV 样例 | source lane 是未来规则地址；dependent lane 是读取 source value 后计算出的间接目标。脚本会将 stats 和 CSV 条目逐个计数比较。 |
| `8. VIR` | `mask failures`、`multi-lane groups`、`maximum group width`、control fallback | VIR 把 ready 且 PC 相同的 lane 合并。Camel 使用 64-bit element 和 512-bit chunk，所以最大宽度是 8。`multi-lane groups>0` 才证明不是每个 lane 都单独执行。若 initial VIR audit 有 unsupported semantic，必须同时看到 persistent continuation fallback 为 0，才说明 source response 进入了新的持久化 helper 路径，而不是临时单 lane fallback。 |
| `9. Helper uop` | `decoded/issued/completed`、fetch/decode | 三个数量相等表示每个 helper uop 都走完生命周期。这里是独立 helper 的轻量 front-end 和 `DVRDynUop`，不是主线程的 DynInst/ROB/commit。 |
| `10. FU` | `requests/grants/stalls`、ALU/shift/multiply chunks | helper uop 先申请共享 FU，再获得 grant。Camel 的计算主要是 ALU；`vector FU conflict cycles=0` 只表示 Camel 主线程没有 SIMD 指令，不能证明 SIMD 竞争已被测试。 |
| `11. LSQ / cache` | source/dependent generated、issued、completed | source 和 dependent 是两种请求。总生成量应满足 `source generated + dependent generated = total issued = total completed`；source 必须使用真实返回值，dependent 才能继续 replay。 |
| `11b. Prefetch quality` | accuracy、coverage、timeliness、pollution | 这是“发出请求”之后的效果检查。coverage 表示减少了多少原本会发生的 miss；timeliness 表示是否及时；pollution 表示预取是否挤掉了有用 cache line。 |

当前 Camel 结果的简化读法：

```text
RPT candidates                 8546
Discovery starts/completions   2082/2074
discoveries with FLR           2055
loop-bound matches             2024
source/dependent lanes         184120/8475
VRAT programs/writes           2024/368240
DynUop decoded/issued/done     205098/205098/205098
VIR mask checks/failures       205098/0
multi-lane max group           8
source/dependent issued/done   184120/184120, 8475/8475
Full Vector speedup            1.034017x
```

因此可以得出：Camel 已经验证普通 DVR data path 有实际执行和约 `1.034x`
性能收益；它没有验证 NDM flatten，也没有验证主线程 SIMD 与 helper 的 FU
竞争。当前 Camel 的 initial VIR audit 仍会对部分 captured semantic 产生
unsupported 计数，并通过显式 source-helper control fallback 继续执行；这部分
不能宣称为完整 initial VIR 语义覆盖。不过 persistent continuation 的 fallback、
timeout 和 stack overflow 都应为 0，且本轮满足这一条件。另有 1 次 recorder
overflow，应在后续长模板实验中消除。当前结果最准确的命名仍是
`DVR-Offload+Discovery` 或 `ISA-adapted execution-driven DVR`，不是完整
Sniper/DynInst bit-exact reproduction。
