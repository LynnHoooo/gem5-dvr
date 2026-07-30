# gem5 DVR 复现实施记录

## 当前结论

主线已从 Sniper 6.0 切换到 `code/gem5-runahead-dev-pre`。该代码库在 PRE
基础上加入了 RISC-V DVR 原型。Stage 1–12 已通过当前树的完整非 QUICK 回归，
包括 actual-value predicate 和质量 tracker 独立 smoke。
通用 uop evaluator 和
两层 Nested Controller 已接入 CPU 提交生命周期和独立 child
taint/recorder/VRAT/VIR/replay context；Stage 13 已验证真实 nested helper memory。

## 论文 Table 1

| 项目 | 论文配置 | gem5 映射状态 |
|---|---:|---|
| 核心 | 4 GHz OoO | 已映射并验证 |
| ROB | 350 | 可精确映射 `numROBEntries` |
| IQ / LQ / SQ | 128 / 128 / 72 | 可精确映射 |
| 宽度 | fetch/dispatch/rename/commit 均为 5 | 可精确映射 |
| 前端深度 | 15 stages | 用分支错误恢复延迟校准，非逐级一一对应 |
| 分支预测 | 8 KB TAGE-SC-L | gem5 已提供 `TAGE_SC_L_8KB` |
| L1I | 32 KB, 4-way, 2 cycles | 可精确映射 |
| L1D | 32 KB, 8-way, 4 cycles, 24 MSHR | 可精确映射 |
| L1D 预取 | stride, 16 streams | 使用 gem5 StridePrefetcher 并校准表项 |
| L2 | 256 KB, 8-way, 8 cycles | 可精确映射 |
| L3 | 8 MB, 16-way, 30 cycles | 已建立 classic cache 三层结构 |
| 内存 | 50 ns，51.2 GB/s | 已用 `SimpleMemory` 延迟与带宽模型映射 |

## 已完成：Stage 1 - RPT 步幅检测

已实现论文 Discovery Mode 的入口结构：

- 32-entry Reference Prediction Table；
- 每项保存 load PC、上次地址、带符号 stride、2-bit confidence 和替换年龄；
- 相同非零 stride 连续出现后形成 DVR candidate；
- 只采集主线程 load，排除 PRE 产生的瞬态访问；
- 参数：`enableDVR`、`dvrRPTEntries=32`、`dvrMaxLanes=128`；
- 统计：`dvrLoadsObserved`、`dvrStrideCandidates`。

验证微基准为 `benchmarks/dvr_stride.c`。服务器 smoke test 结果：

```text
DVR_STAGE1_SMOKE_PASSED loads=174317 candidates=173525
```

## 已完成：Stage 2 - Table 1 baseline

独立配置脚本为 `configs/dvr/table1_se.py`，不修改 gem5 通用的
`configs/example/se.py`。当前配置包括：

- 4 GHz 单核 `DerivO3CPU`，fetch/decode/rename/dispatch/issue/writeback/commit 均为 5-wide；
- 350-entry ROB、128-entry IQ、128-entry LQ、72-entry SQ；
- 256 个整数、256 个浮点、128 个向量物理寄存器；
- 原生 `TAGE_SC_L_8KB`；
- 4 个整数加法、1 个整数乘法、1 个整数除法、独立浮点单元，以及论文数量的五类向量 FU；
- 32 KiB L1I、32 KiB L1D、256 KiB 私有 L2 和 8 MiB L3；
- L1D 24 MSHR 和 16-entry gem5 stride PC table；
- 50 ns、51.2 GB/s `SimpleMemory`。

服务器验证命令：

```bash
cd ~/buckyball
nix develop
~/dvr-repro/scripts/run_remote_table1_smoke.sh
```

结果：

```text
Exiting @ tick 82979500 because exiting with last active thread context
TABLE1_BASELINE_SMOKE_PASSED out=/home/lynnhoo/dvr-repro/results/table1-baseline-smoke
```

验收脚本从实际生成的 `config.ini` 检查核心频率、全部队列和宽度、
物理寄存器、分支预测器、各级 cache、stride 表项数和内存参数，而不只检查
Python 源码。

### gem5 映射边界

- 论文的 15-stage front end 不能用 gem5 O3 的单个参数精确表达；目前保留
  gem5 的阶段间延迟，后续用分支错误预测惩罚实验校准。
- Table 1 的 16 stride streams 映射为 16-entry PC table；预取 degree 暂设为 4，
  因论文没有给出每次触发的请求数。
- `SimpleMemory` 提供请求排队和带宽限制，但不等同于 Sniper 的
  request-based contention 实现。因此这是机制/趋势复现，不是逐周期等同复现。
- RISC-V 后端当前不会执行论文 x86/AVX 指令；向量 FU 是为后续 DVR 内部
  vector uop 建模预留的资源约束。

## 已完成：Stage 3 - Discovery Mode 状态机

新增 commit-ordered `DVRDiscoveryController`，状态转换为：

```text
Idle --(RPT candidate)--> Armed --(candidate load commits)--> Discovering
Discovering --(same load PC commits again)--> Idle / Completed
Discovering --(instruction limit)-----------> Idle / TimedOut
```

关键实现约束：

- RPT 在 load execute 时产生 candidate，但携带动态 instruction sequence number；
- 只有该 sequence number 的 load 真正 commit 后才开始 discovery；
- discovery 长度按已提交的架构指令统计，不受错误路径和 O3 执行乱序影响；
- 下一次相同 load PC commit 时正常结束；
- `dvrDiscoveryMaxInsts` 默认 512，可配置并有独立 timeout 路径；
- discovery 期间忽略新的 RPT arm，第一版只允许一个活动 discovery。

新增统计：

- `dvrDiscoveryStarts`
- `dvrDiscoveryCompletions`
- `dvrDiscoveryTimeouts`
- `dvrDiscoveredInstructions`

服务器验证：

```bash
cd ~/buckyball
nix develop
~/dvr-repro/scripts/run_remote_dvr_stage3_smoke.sh
```

结果：

```text
DVR_STAGE3_SMOKE_PASSED starts=5080 completions=5080 \
instructions=25400 timeouts=5222
```

其中正常测试使用 512 条指令上限；第二组将上限设为 1，专门验证 timeout
路径。两组均在 Table 1 baseline 配置下执行到程序正常退出。

## Stage 4 - 32-register VTT 与 FLR

新增 `DVRVectorTaintTracker`：

- discovery 开始时清空 32-bit VTT，并 taint initiating stride load 的目标寄存器；
- 任一整数源寄存器 tainted 时，将 taint 传播到目标整数寄存器；
- 无 tainted source 的指令会清除其目标寄存器原有 taint；
- load 的地址源寄存器 tainted 时，将该 load PC 写入 Final Load Register；
- discovery 正常结束时检查 FLR，timeout/结束后清空 VTT 与 FLR。

论文基于 x86 的 16 个架构整数寄存器，因此原文硬件开销为 16 bit。当前 gem5
RISC-V 原型扩展为 32 bit，覆盖 x0–x31；这是 ISA 适配，不应当写成论文原硬件
开销的逐位复刻。当前树的专用回归结果为：

专用微基准 `benchmarks/dvr_dependent.c` 构造
`indices[i] -> payload[indices[i]]` 依赖链。验证结果：

```text
DVR_STAGE4_SMOKE_PASSED tainted=12401 dependent_loads=2396 with_flr=2396
```

## 已完成：Stage 5 - backward branch 与循环边界

新增 `DVRLoopBoundDetector`，在每次 FLR 更新时清空 LCR/SBB 等价状态，并寻找：

- conditional direct backward branch；
- branch target 不晚于 initiating stride load；
- branch PC 位于当前 FLR 之后，即回边包围完整的 trigger-to-FLR 链。

论文 x86 模型从 compare 的目标 flags 和源寄存器建立 LCR。RISC-V branch
直接比较两个整数源，没有独立 flags/compare 目标，因此当前后端把 backward
branch 的最多两个整数源寄存器保存为 loop-bound candidates。这保留了 LCR/SBB
用途，但不是 x86 数据通路的逐位复制。

验证结果：

```text
DVR_STAGE5_SMOKE_PASSED backward=5213 bounds=2396 discoveries=2396
```

对应结果目录：

```text
/home/lynnhoo/dvr-repro/results/dvr-stage4-taint-flr
/home/lynnhoo/dvr-repro/results/dvr-stage5-loop-bound
```

## 已完成：Stage 6 - remaining iterations 与 lane count

按论文 4.1.3 的双检查点方法，在 initiating stride load commit 和下一次同 PC
load commit 时各保存一次架构整数寄存器值。由于 `instDone()` 位于 commit rename
map 更新之前，快照会用当前提交指令的 destination physical register 覆盖旧映射，
避免边界处出现一条指令的滞后。

对 loop branch 的两个 bound-source candidates：

- 一个值在两个检查点之间保持不变，作为 loop bound；
- 另一个值发生变化，其有符号差值作为 loop increment；
- 根据方向计算 `ceil(distance / abs(increment))`；
- active lanes 为 remaining iterations 与 `dvrMaxLanes=128` 的较小值；
- 无法形成“一常量、一变量”匹配时仍记录 128-lane fallback 统计，但为避免
  未知循环边界越界，当前 CPU 不启动该次 memory helper。

验证结果：

```text
DVR_STAGE6_SMOKE_PASSED matches=2396 fallbacks=2817 \
samples=5213 lanes=665909
```

2396 个 discovery 成功完成寄存器值匹配；2817 个没有可信边界的 discovery
只计入 fallback 统计并抑制 memory helper。结果目录：

```text
/home/lynnhoo/dvr-repro/results/dvr-stage6-lane-count
```

## 已完成：Stage 7 - 128-lane L1D timing prefetch

在 discovery 完成且存在 FLR 后，helper 根据当前 trigger 地址、已学习 stride 和
active lane count 生成最多 128 个虚拟地址。地址先经当前线程的 DTLB 翻译；
source 以带 prefetch 标记的 `ReadReq`、dependent 以 `SoftPFReq` 送入 O3 LSQ
data-cache port；端口 backpressure、翻译
fault、完成响应均有独立统计。验证结果：

```text
DVR_STAGE7_SMOKE_PASSED generated=305333 issued=110005 \
completed=110005 dropped=292747 faults=0 \
source_faults=0 dependent_faults=0
```

source helper 使用带 `Request::PREFETCH` 标记的 `ReadReq`，因为 trigger-to-FLR
重放必须取得实际 load bytes；dependent helper 使用 `SoftPFReq`，只保留 cache
副作用。此前两级都使用 `SoftPFReq` 时，source response payload 未定义并产生
54,436 个错误 dependent 地址。当前脚本强制要求两类 translation fault 都为 0。

## 已完成：Stage 8 - 两级依赖预取

针对 `indices[i] -> payload[indices[i]]`，source prefetch 返回 8-byte 数据后，
CPU 将真实值写入该 lane 的寄存器快照，并重放 FLR 之前记录的
`C.SLLI → C.ADD → C.LD`，由最后一个 load-address uop 产生 dependent
`SoftPFReq`。模板严格在 FLR 截断，不执行主线程中消费 FLR 的 reduction 指令。

```text
DVR_STAGE8_SMOKE_PASSED relations=1 generated=97419 issued=12586 \
completed=12586 replay_supported=7188 replay_attempts=97419 \
replay_targets=97419 replay_fallbacks=0
```

## 已完成：Stage 9 - baseline/DVR 自动对照

同一二进制、同一 Table 1 gem5 配置，只切换 `--dvr`：

| 指标 | Baseline | DVR | 变化 |
|---|---:|---:|---:|
| cycles | 2,462,727 | 2,462,523 | speedup 1.000083× |
| demand L1D misses | 250,819 | 221,823 | -11.56% |

该微基准先初始化整个 payload，且多数请求会在较低层 cache 命中，所以目前应把
11.56% 的 demand miss 降幅视为功能证据，不把很小的周期差包装成论文级性能复现。

## Stage 10 - 8-uop recorder、VRAT 与 VIR 骨架

新增结构：

- 最多 8 条 trigger-to-FLR tainted uop 的记录器；
- 32 个 RISC-V 架构整数寄存器、8 个 16-lane chunk、128 个逻辑 vector
  physical ID 的 VRAT；
- 两个 64-bit active-mask word，以及逐 uop、逐 chunk 的 VIR issue/execute 状态；
- discovery 完成时按实际 active lanes 构建映射并驱动 VIR 结构计数。

```text
DVR_STAGE10_SMOKE_PASSED uops=17614 overflows=0 programs=2396 \
vrat_allocations=95470 vir_issues=95470 vir_executions=95470
```

这里的 VRAT/VIR 仍是微结构/调度原型，但 Stage 8 的实际 memory helper 已执行
逐 lane 寄存器值和异步 source response。当前 evaluator 覆盖测试所需的
`ADD/ADDI/SLLI/ANDI/load-address` 以及 `C.ADD/C.SLLI/C.LD`；其他语义会明确
回退到训练关系，不能据此声称支持任意 RISC-V 指令链。

结果目录：

```text
/home/lynnhoo/dvr-repro/results/dvr-stage8-dependent-prefetch
/home/lynnhoo/dvr-repro/results/dvr-stage9-baseline
/home/lynnhoo/dvr-repro/results/dvr-stage9-dvr
/home/lynnhoo/dvr-repro/results/dvr-stage10-vrat-vir
```

## Stage 11 - 控制流验收

`scripts/run_remote_dvr_stage11_control_flow.sh` 对正常和强制 timeout 两组运行做
硬性断言：

- actual-value divergent generation 大于零；
- 每个 divergence 最终必须 reconverge 或被后继 generation 显式 abandon；
- distinct predicate paths 至少为 2；
- 至少训练两个地址关系并产生 dependent prefetch；
- 正常组 timeout 和 reconvergence-stack overflow 为零；
- `dvrHelperMaxUops=1` 时必须发生 timeout，且不能产生 prefetch。

这些断言比单看统计字段存在更强，但仍只覆盖 `dvr_divergent.riscv` 微基准，不足以
证明任意多分支程序或 Nested DVR 正确。

当前树在主线程优先节流和 predicate 判别位修复后已经得到：

```text
relations=2 distinctPredicatePaths=2
divergent=3019 reconvergences=604 predicateGenerationAbandons=2415
predicateSelections=238232 predicateMisses=100
dependentPrefetchGenerated=238232
forcedTimeouts=1965 forcedGenerated=0
```

3019 个 divergence 全部由真实 source response 与 learned discriminator 形成，
其中 604 个收齐 lane 后 reconverge，2415 个因下一轮 launch 替换而 abandon。
正常组和强制 timeout 组均已通过脚本中的硬断言。

## 通用 RISC-V uop template 与 Nested Controller

Recorder 现在保存原始 encoding、整数源/目标寄存器和立即数，并能无架构副作用
执行 `ADD`、`ADDI`、`SLLI`、`ANDI`、load effective-address，以及测试使用的
`C.ADD/C.SLLI/C.LD`。CPU 已将逐 lane register snapshot 和异步 source response
接入 evaluator；Stage 8/10 硬断言得到 `attempts=targets=97419`、`fallbacks=0`。
未知语义仍会显式回退，当前不是任意 opcode 执行器。

`dvr_nested.hh/.cc` 新增了最多两层、LIFO 完成、父子 ID 校验和逐 commit timeout
的 Nested Controller，通过独立 C++ smoke test 和 gem5 完整链接，并已在提交
路径中产生 root/child start、child recurrence completion 与 parent reset 统计。
child 现在独立保存 taint、recorder、loop-bound、register snapshot、VRAT、VIR 和
replay template，并以 append-only 方式生成最多 128 lane helper。共享的 relation
predictor 和 physical request queue 不属于独立 context。

Stage 13 的真实结果为 `contexts=440, programs=2, vrat=80, vir=80,
generated=256, issued=251, completed=251`；因此 nested memory execution 已在专用
微基准上成立，但尚未扩展为 GAP/论文 benchmark 性能结论。

## 回归入口

```bash
# 短回归；用于快速发现结构性退化
QUICK=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh

# 完整 Stage 1–12；默认先构建
~/dvr-repro/scripts/run_remote_dvr_regression.sh

# 保留完整测试、跳过构建
SKIP_BUILD=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

`QUICK=1` 明确跳过 Stage 5–9 和 Stage 11，因此不能用作完整通过证据。完整模式
串联现有每个 Stage 的自校验脚本，失败立即停止，并把逐步日志及摘要保存到
`~/dvr-repro/results/dvr-regression-logs/`。

当前源码的完整非 QUICK 回归已通过，摘要为：

```text
/home/lynnhoo/dvr-repro/results/dvr-regression-logs/20260730T102004Z.summary
```

## 后续实现顺序

1. 扩展逐 lane evaluator 的 RV64/RVC opcode，并逐步缩小仿射 fallback 覆盖面。
2. 把真实 value-predicate 扩展到更多 branch opcode和复杂路径。
3. 在主线程优先 cache-port 节流和质量 proxy 上补执行端口与严格质量统计。
4. 完整回归后进入缩小版 GAP 和四档消融实验。
