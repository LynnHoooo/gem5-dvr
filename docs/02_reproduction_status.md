# Decoupled Vector Runahead 复现状态

更新时间：2026-07-30

## 结论

`code/gem5-runahead-dev-pre` 是 gem5/RISC-V 上的 DVR 机制原型：已经具备
stride trigger、commit-ordered Discovery、32-register VTT/FLR、loop-bound 和
lane-count 推断、最多 128-lane 的真实 cache timing 预取、两级 dependent
prefetch、8-uop recorder、VRAT/VIR 与控制流验收结构。它不是论文
Sniper/x86/AVX-512 后端或论文绝对性能数字的逐项复刻。

当前源码已通过 32-register VTT、两级 dependent prefetch、VRAT/VIR 和多路径
控制流回归；通用 uop evaluator 与两层 Nested Controller 已编译，但尚未连接成
完整的逐 lane/Nested helper memory 后端。

## Stage 1–11 证据台账

| Stage | 验收点 | 已知证据 | 当前判定 |
|---|---|---:|---|
| 1 | 32-entry RPT / stride candidate | loads=174317, candidates=173525 | 已通过 |
| 2 | Table 1 风格配置 | config.ini 自动逐项检查通过 | 已通过 |
| 3 | Discovery 正常结束和强制 timeout | completions=5080, timeouts=5222 | 已通过 |
| 4 | 32-register VTT / FLR | tainted=12124, dependent loads/FLR=2291 | 当前树通过 |
| 5 | backward branch / loop bound | 历史 bounds=3897 | 当前树待重跑 |
| 6 | remaining iterations / lane count | 历史 active lanes=607699 | 当前树待重跑 |
| 7 | 最多 128-lane `SoftPFReq` timing 注入 | 历史 completed=99304 | 当前树待重跑 |
| 8 | source response 驱动 dependent prefetch | completed=8198 | 当前树通过 |
| 9 | 同配置 baseline vs DVR | misses 250819→170348 (-32.08%) | 当前树通过 |
| 10 | recorder / VRAT / VIR chunk | programs=2291, executions=91195 | 当前树通过 |
| 11 | predicate paths / reconvergence / timeout | 2 paths，5448 divergent=reconvergences，forced timeout=2884/generated=0 | 当前树通过 |

Stage 9 的最新周期为 baseline 2,462,727、DVR 2,462,511，
speedup 1.000088×。它证明该微基准的 demand-miss coverage，不证明论文报告的
整体 speedup。

Stage 11 正常预算组的当前树证据为：

```text
starts=8168 completions=8168 abandons=4634 programs=5449
relationsTrained=2 distinctPredicatePaths=2
dependentPrefetchGenerated=368572
divergent=5448 reconvergences=5448
predicateSelections=368572 predicateMisses=149
forcedTimeouts=2884 forcedGenerated=0
```

Stage 11 的正常和强制 timeout 两组均由脚本完成硬断言。

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

1. 将通用 RISC-V uop evaluator 接入逐 lane VRAT values 与 load response，
   完全替代仿射 fast path。
2. 将实际 value-predicate 路径选择扩展到更多 branch opcode。
3. 将两层 Nested Controller 接入 CPU、独立 VRAT/VIR 和 helper memory。
4. 把主线程优先 cache-port 节流扩展为执行端口级竞争。
5. 在质量 proxy 上增加严格 accuracy/coverage/timeliness/bandwidth/pollution。
6. 缩小版 GAP workload。
7. Baseline → PRE → Offload/Discovery → Nested DVR 消融。
8. 基于新完整回归和消融数据生成最终 Markdown 实验报告。

推荐对外表述：

> We implement an ISA-adapted prototype of DVR on a RISC-V
> microarchitecture.
