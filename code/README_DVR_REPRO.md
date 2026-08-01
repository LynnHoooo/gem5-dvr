# gem5 RISC-V DVR 复现代码导览

> 更新时间：2026-07-30
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
5. `src/cpu/o3/cpu.cc` 中的 `CPU::instDone()`：在 commit 阶段把上述结构串起来。
6. `src/cpu/o3/cpu.cc` 中的 `launchDVRStridePrefetches()`、
   `serviceDVRPrefetchQueue()`、`completeDVRPrefetch()`：128-lane helper 和
   dependent prefetch 的真实 cache timing 路径。
7. `src/cpu/o3/lsq_unit.cc`：主线程 load 在 LSQ 发射时训练 RPT。
8. `src/cpu/o3/lsq.cc`：截获并消费 DVR 的 cache response。
9. `configs/dvr/table1_se.py`：论文 Table 1 风格的 gem5 配置和 DVR 参数。

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
- `DVRDiscoveryController`：`Idle → Armed → Discovering`。
- discovery 在下一次相同 trigger PC commit 时完成。
- 最多记录 512 条 committed instruction。
- `Abandoned` 路径用于释放由 speculative/squashed load 产生的 armed trigger。

### 3.3 VTT、FLR 和 Loop Bound

- `DVRVectorTaintTracker`：RISC-V x0–x31 的 32-bit VTT。
- tainted source 向 destination 传播 taint。
- 地址源 tainted 的 load 更新 FLR。
- `DVRLoopBoundDetector`：识别包围 trigger-to-FLR 的 backward branch。
- 两次寄存器快照推断 bound、increment、remaining iterations 和 active lanes。

论文的 16-bit VTT 对应 x86 的 16 个架构整数寄存器；本原型扩展到 32 bit，
是对 RISC-V 32 个整数寄存器的 ISA 适配。

### 3.4 Recorder、VRAT 和 VIR

- `DVRInstructionRecorder`：最多保存 8 条 tainted uop template。
- `DVRVectorRenameTable`：32 个 RISC-V architectural integer registers、
  8 个 16-lane chunk、128 个逻辑 vector physical ID。
- `DVRVectorInstructionRegister`：两个 64-bit active-mask word。
- 按 16-lane chunk 记录 issue/execute。
- 8-entry reconvergence stack。
- 200-helper-uop termination budget。

这里的 VRAT/VIR 已有真实状态和运行统计，但尚未成为任意 RISC-V 指令的完整
逐 lane 数据执行后端；当前真实 dependent address 执行仍以 discovery 学习出的
仿射关系为主要 fast path。

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

## 4. 当前验证状态

| Stage | 内容 | 当前状态 | 已获得的证据 |
|---|---|---|---|
| 1 | RPT stride detection | 通过 | 174317 loads，173525 candidates |
| 2 | Table 1 baseline config | 通过 | 自动配置 smoke test |
| 3 | Discovery/timeout | 通过 | 5080 completions，5222 forced timeouts |
| 4 | VTT/FLR | 当前树通过 | 12401 tainted、2396 dependent loads/FLR |
| 5 | Loop Bound | 当前树通过 | 2396 bounds / discoveries |
| 6 | Lane inference | 当前树通过 | 2396 matches，665909 total active lanes |
| 7 | 128-lane cache injection | 当前树通过 | 110005 timing requests completed；source/dependent translation faults 均为 0 |
| 8 | 真实逐 lane dependent replay | 当前树通过 | 97419 replay attempts/targets，0 fallback；12586 dependent requests completed |
| 9 | Baseline vs DVR | 当前树通过 | demand L1D miss 降低 11.56% |
| 10 | 8-uop recorder/VRAT/VIR | 当前树通过 | 2396 programs，95470 VIR executions；真实 replay 守恒断言通过 |
| 11 | actual-value predicate/reconvergence/timeout | 当前树完整通过 | divergent=3019，reconverged=604，abandoned=2415；forced timeout=1965/generated=0 |
| 12 | predicate/quality 独立严格 smoke | 当前树通过 | actual-value mask 与质量计数器 `-Werror` smoke |

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

Stage 8/10 对真实 trigger-to-FLR replay 的最新硬断言结果：

```text
replay_supported=7188 replay_unsupported=0
replay_unstable_inputs=0
replay_attempts=97419 replay_targets=97419 replay_fallbacks=0
```

该测试中的 RVC 链为 `load → C.SLLI → C.ADD → C.LD`。模板在 FLR 截断，
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

1. 将已验证的逐 lane evaluator 扩展到更多 RV64/RVC 整数、比较和地址生成 opcode；
   仿射逻辑仅保留为 unsupported 链的显式 fallback。
2. 将已验证的实际 value-predicate 路径选择进一步扩展到任意 branch opcode，
   并让 replay 直接执行逐 lane predicate。
3. 将当前主线程优先 cache-port 节流扩展为执行端口级资源竞争模型。
4. 将已完成的严格质量 tracker 接到 L1 tag lookup/fill/victim/invalidate，
   使 accuracy、coverage、timeliness 和 pollution 可用于 workload 报告。
5. 缩小版 GAP workload。
6. Baseline、PRE、Offload/Discovery、Nested DVR 消融。
7. 最终实验报告（Stage 1–13 验收入口已经补齐）。

Nested 专用验收：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage13_nested.sh
```

服务器最新证据为 `contexts=440, programs=2, vrat=80, vir=80,
generated/issued/completed=256/251/251`，并且真实 child replay 为
`attempts=210, targets=210, fallbacks=0, nested_dependent=210`。child 独立持有 taint、recorder、
loop-bound、register snapshots、VRAT、VIR 和 replay template；relation predictor
与物理 helper queue 仍由 root/child 共享。

最新完整回归已经通过：

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
- 已实现最多 128 lane、8-uop recorder、VRAT/VIR 状态和 200-uop timeout。
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

#### 9.3.2 当前 Nested Controller 不等于论文的 Nested Discovery Mode

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

当前实现主要是：root discovery 中发现另一个 candidate，candidate commit 后建立
独立 child context，child recurrence 后按 child stride 生成 helper。它证明了两层
独立 discovery/replay context 能发送真实内存请求，但尚缺 `<64 lane` 门槛、
branch inversion、IR/ILR、outer invocation vectorization 和 inner-lane flatten。

因此，当前 Stage 13 应描述为“两层独立上下文和真实 helper 验证”，不能描述为
论文 4.3 节 NDM 的完整复现。

#### 9.3.3 helper 尚未完整共享论文中的执行资源

论文要求 helper 与主线程共享执行单元，并且只有在相同端口没有 main-thread
ready instruction 时才能发射。当前实现已经具有主线程优先的 cache/data-port
节流，但地址计算主要由 CPU 侧 C++ evaluator 完成，VIR 尚未作为真实 gem5 FU
pipeline 的有序发射后端。

目前未完整建模：

- IntALU、shift、multiply、vector FU 的占用；
- helper uop latency；
- per-port main-thread priority；
- source load → dependent ALU → next load 的调度时间；
- helper front-end buffer 对 fetch/decode 带宽的影响。

这会影响最终 performance、MLP 和 timeliness，必须在正式 workload 实验前补齐
至少一个可审计的资源 token/port contention 模型。

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
~/dvr-repro/scripts/run_remote_dvr_stage14_ndm_control.sh
```

当前 Stage 14 是控制语义和可观测性骨架，尚未实现论文要求的 branch-direction
inversion、outer-lane vectorization、每个 outer invocation 的 inner bound 收集和
flatten-to-128。服务器完成构建与回归前，其状态应写为“已实现，待远端验证”。

## 11. Stage 15：资源竞争统计与固定构建环境

Stage 15 已加入回归脚本，统计 `dvrHelperIssueCycles`、`dvrResourceConflicts`、
`dvrPrefetchesSuppressedMainThread` 和 `dvrPrefetchesIssued`。运行：

```bash
bash ~/dvr-repro/scripts/run_remote_dvr_stage15_resource_smoke.sh
```

通过条件是模拟周期、helper issue 周期和 DVR 请求均大于零，且 helper issue 周期不
超过总周期。2026-08-01 的服务器验证已经通过：

```text
DVR_STAGE15_RESOURCE_PASSED cycles=2462727 helper_issue_cycles=9 conflicts=0 main_thread_suppressed=1 dvr_issued=9
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
