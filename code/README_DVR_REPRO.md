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
3. `src/cpu/o3/cpu.cc` 中的 `CPU::instDone()`：在 commit 阶段把上述结构串起来。
4. `src/cpu/o3/cpu.cc` 中的 `launchDVRStridePrefetches()`、
   `serviceDVRPrefetchQueue()`、`completeDVRPrefetch()`：128-lane helper 和
   dependent prefetch 的真实 cache timing 路径。
5. `src/cpu/o3/lsq_unit.cc`：主线程 load 在 LSQ 发射时训练 RPT。
6. `src/cpu/o3/lsq.cc`：截获并消费 DVR 的 cache response。
7. `configs/dvr/table1_se.py`：论文 Table 1 风格的 gem5 配置和 DVR 参数。

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
│       │   ├── dvr_nested.hh / .cc       # 两层 Nested 控制器（尚未接 CPU）
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
│   └── run_remote_dvr_regression.sh     # QUICK/full 一键回归
└── docs/
    ├── 02_reproduction_status.md
    ├── 04_gem5_dvr_implementation.md
    ├── 05_gap_experiment_plan.md
    └── 06_nested_dvr_design.md
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
| 4 | VTT/FLR | 当前树通过 | 12124 tainted、2291 dependent loads/FLR |
| 5 | Loop Bound | 历史回归通过；当前树待全回归 | 3897 bounds |
| 6 | Lane inference | 历史回归通过；当前树待全回归 | 607699 total active lanes |
| 7 | 128-lane cache injection | 历史回归通过；当前树待全回归 | 99304 timing requests completed |
| 8 | 两级 dependent prefetch | 当前树通过 | 8198 dependent requests completed |
| 9 | Baseline vs DVR | 当前树通过 | demand L1D miss 降低 32.08% |
| 10 | 8-uop recorder/VRAT/VIR | 当前树通过 | 2291 programs，91195 VIR executions |
| 11 | predicate path/reconvergence/timeout | 当前树完整通过 | 2 relations、2 paths、5448 divergent=5448 reconvergences；forced timeout 2884、generated 0 |

Stage 9 的当前稳定结果：

```text
baseline_cycles=2462727
dvr_cycles=2462511
speedup=1.000088
baseline_demand_l1d_misses=250819
dvr_demand_l1d_misses=170348
miss_reduction=32.08%
```

该微基准证明了 cache miss coverage，但不代表已经复现论文的 2.4× 绝对结果。

Stage 11 在加入主线程优先节流和 predicate 判别位修复后的最新完整结果：

```text
starts=8168 completions=8168 abandons=4634 programs=5449
relations=2 distinct_predicate_paths=2
divergent=5448 reconvergences=5448
predicate_selections=368572 predicate_misses=149
dependent_prefetch_generated=368572
forced_timeouts=2884 forced_generated=0
```

正常组满足多 relation、多 path 和分歧/重汇合数量相等的断言；强制
`dvrHelperMaxUops=1` 的第二组也证明 timeout 后不会发出 helper prefetch。

新增的质量/节流 proxy：

```text
source_issued=368887 source_completed=368883
dependent_issued=29812 dependent_completed=29812
queue_peak=133 suppressed_main_thread=15862
possibly_useful=33607 late=697
```

最后 4 个 source request 在程序退出时仍 outstanding，因此 issued 与 completed
相差 4；总 issued 严格等于 source issued 与 dependent issued 之和。

## 5. 还缺什么

按重要程度排列：

1. 将已支持的 `ADD/ADDI/SLLI/ANDI/load-address` evaluator 接入逐 lane
   register values 和异步 load response，替代仿射 fast path。
2. 将已验证的实际 value-predicate 路径选择进一步扩展到任意 branch opcode。
3. 把已实现的两层 Nested Controller 接入独立 VRAT/VIR/helper memory context。
4. 将当前主线程优先 cache-port 节流扩展为执行端口级资源竞争模型。
5. 在现有 possibly-useful/late proxy 上增加严格 accuracy、coverage、
   timeliness、bandwidth、pollution 统计。
6. 缩小版 GAP workload。
7. Baseline、PRE、Offload/Discovery、Nested DVR 消融。
8. 最终实验报告（Stage 1–11 一键回归入口已经补齐）。

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

完整回归（默认先编译，再串联 Stage 1–11）：

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
