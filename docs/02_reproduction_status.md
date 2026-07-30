# Decoupled Vector Runahead 复现状态

更新时间：2026-07-30

## 结论

`code/gem5-runahead-dev-pre` 是 gem5/RISC-V 上的 DVR 机制原型：已经具备
stride trigger、commit-ordered Discovery、32-register VTT/FLR、loop-bound 和
lane-count 推断、最多 128-lane 的真实 cache timing 预取、两级 dependent
prefetch、8-uop recorder、VRAT/VIR 与控制流验收结构。它不是论文
Sniper/x86/AVX-512 后端或论文绝对性能数字的逐项复刻。

当前源码已通过 32-register VTT、两级 dependent prefetch、VRAT/VIR 和多路径
控制流回归；通用 uop evaluator 已用于逐 lane replay。两层 Nested Controller
已接入真实提交生命周期和独立 child taint/recorder/VRAT/VIR/replay context，
Stage 13 已证明 child helper 经 L1D timing port 发出并返回。

## Stage 1–13 证据台账

| Stage | 验收点 | 已知证据 | 当前判定 |
|---|---|---:|---|
| 1 | 32-entry RPT / stride candidate | loads=174317, candidates=173525 | 已通过 |
| 2 | Table 1 风格配置 | config.ini 自动逐项检查通过 | 已通过 |
| 3 | Discovery 正常结束和强制 timeout | completions=5080, timeouts=5222 | 已通过 |
| 4 | 32-register VTT / FLR | tainted=12401, dependent loads/FLR=2396 | 当前树通过 |
| 5 | backward branch / loop bound | bounds=discoveries=2396 | 当前树通过 |
| 6 | remaining iterations / lane count | matches=2396, active lanes=665909 | 当前树通过 |
| 7 | 最多 128-lane timing 注入 | completed=110005, source/dependent translation faults=0 | 当前树通过 |
| 8 | source response 驱动真实逐 lane replay | attempts=targets=97419, fallback=0, dependent completed=12586 | 当前树通过 |
| 9 | 同配置 baseline vs DVR | misses 250819→221823 (-11.56%) | 当前树通过 |
| 10 | recorder / VRAT / VIR chunk | programs=2396, executions=95470, replay守恒断言通过 | 当前树通过 |
| 11 | actual-value predicate / reconvergence / timeout | divergent=3019, reconverged=604, abandoned=2415；forced timeout=1965/generated=0 | 当前树通过 |
| 12 | predicate/quality 独立 smoke | actual lane masks、严格事件计数 | 当前树通过 |
| 13 | Nested 独立执行上下文和真实 helper | contexts=440, programs=2, generated/issued/completed=256/251/251 | 当前树通过 |
| 14 | NDM `<64 lanes` 控制、IR/ILR、outer candidate、fallback/timeout | 等待服务器运行新增三组验收 | 已实现，待远端验证 |

Stage 9 的最新周期为 baseline 2,462,727、DVR 2,462,523，
speedup 1.000083×。它证明该微基准的 demand-miss coverage，不证明论文报告的
整体 speedup。

Stage 11 正常预算组的当前树证据为：

```text
starts=8520 completions=8519 abandons=4567 programs=3027
relationsTrained=2 distinctPredicatePaths=2
dependentPrefetchGenerated=238232
divergent=3019 reconvergences=604 predicateGenerationAbandons=2415
predicateSelections=238232 predicateMisses=100
forcedTimeouts=1965 forcedGenerated=0
```

Stage 11 的正常和强制 timeout 两组均由脚本完成硬断言。

2026-07-30 17:27 完成了当前源码的非 QUICK 全回归：

```text
DVR_REGRESSION_PASSED quick=0
summary=/home/lynnhoo/dvr-repro/results/dvr-regression-logs/20260730T102004Z.summary
```

## Stage 14 待验证说明

当前代码新增 NDM 控制语义第一阶段：默认仅在可信 inner lane count `<64` 时启动，
保存 inner trigger PC（ILR 语义）、loop increment（IR 语义），并只接受经过动态
load commit 过滤的 distinct outer stride。NDM 使用独立的提交预算，新增正常、
threshold=1 禁用和 NDM timeout 三组服务器验收。

该阶段尚未实现 branch inversion、outer vectorization、per-invocation inner bound
收集和 flatten-to-128，不能写成完整论文 NDM。服务器验证入口：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage14_ndm_control.sh
```

## 一键复现

在服务器 `pre` 的 `~/buckyball` Nix development shell 中：

```bash
cd ~/buckyball
nix develop
QUICK=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

`QUICK=1` 跳过构建、Stage 5–9 和 Stage 11 的耗时重复仿真，但会运行
Stage 1–4、Stage 10，并执行 Stage 2 的完整配置检查。它适合编辑后的短回归，
不是完整通过证据。

完整回归：

```bash
cd ~/buckyball
nix develop
~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

已有最新二进制时：

```bash
SKIP_BUILD=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

结果默认写入 `~/dvr-repro/results/`；逐步日志和摘要位于
`~/dvr-repro/results/dvr-regression-logs/`。脚本任一验收失败即返回非零。

## 仍未完成

1. 扩展当前已接入 source response 的逐 lane evaluator，使其覆盖更多
   RV64/RVC opcode；当前 unsupported 链仍显式回退仿射关系。
2. 将实际 value-predicate 路径选择扩展到更多 branch opcode。
3. 把主线程优先 cache-port 节流扩展为执行端口级竞争。
4. 将严格质量 tracker 接到 L1 tag/fill/victim/invalidate 回调；当前 workload
   已有严格 issued/completed bytes，其他指标仍不可报告。
5. 缩小版 GAP workload。
6. Baseline → PRE → Offload/Discovery → Nested DVR 消融。
7. 基于新完整回归和消融数据生成最终 Markdown 实验报告。

推荐对外表述：

> We implement an ISA-adapted prototype of DVR on a RISC-V
> microarchitecture.
