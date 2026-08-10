# Camel DVR Validation Report

## 结论摘要

Camel 已在冻结服务器版本上完成功能和多 lane 回归验证。当前结果可以证明：

- helper uop 的 decode、issue、completion 数量守恒；
- dependent prefetch 的生成、发送和完成数量守恒；
- 逐 lane replay load 地址与标量参考一致；
- 16、32、64、128 lane 配置均没有 reference mismatch 或 predicate miss。

这是一份 **RISC-V/gem5 DVR 原型验证报告**。它证明当前 Camel 数据流和 helper 执行路径正确，不等同于论文 x86/Sniper 绝对性能结果的复现。

## 实验配置

| 项目 | 配置 |
|---|---|
| Workload | Camel |
| ISA | RISC-V |
| Simulator | gem5 DerivO3CPU |
| L1D | 32 KiB（当前 frozen Table 1 配置） |
| DVR mode | Full DVR |
| Lane sweep | 16 / 32 / 64 / 128 |
| 输入 | `camel.riscv`, `MAX_KEY=4096` |
| 结果目录 | `/home/lynnhoo/dvr-repro/results/` |
| Git commit | `c551d41` |

## 关键 PC 的 L1D demand miss 变化

Camel 的关键依赖链为：

```text
0x103b4  source  →  0x103ba  dependent
```

下面的 miss 是主线程 demand access 的 miss；helper 自己的 source/dependent miss 另外列出，不能混为主线程 miss。

| PC | 类型 | Baseline demand misses | DVR demand misses | Miss reduction | Baseline miss rate | DVR miss rate |
|---|---|---:|---:|---:|---:|---:|
| `0x103b4` | source | 970 | 150 | 84.54% | 23.68% | 3.66% |
| `0x103ba` | dependent | 109 | 33 | 69.72% | 2.66% | 0.81% |

### 关键 PC 占全部访存指令的比例

这里的分母使用 `system.cpu.commit.loads + commit.stores`，即主线程实际提交的访存指令数；不会把 helper 的 speculative 请求混入“总访存指令”。本次 Camel run 的主线程分母为 8,222。

| PC | 类型 | Baseline 访问数 | DVR 访问数 | 占 Baseline 全部访存 | 占 DVR 全部访存 |
|---|---|---:|---:|---:|---:|
| `0x103b4` | source | 4,096 | 4,097 | 49.82% | 49.83% |
| `0x103ba` | dependent | 4,096 | 4,096 | 49.82% | 49.82% |
| 两个 PC 合计 | source + dependent | 8,192 | 8,193 | 99.64% | 99.65% |

因此这两个 PC 不是“很少的采样点”，而是 Camel 访存指令的主要组成部分。它们合计约占全部主线程访存指令的 99.6%。

计算方式：

```text
miss rate = demand_misses / (demand_hits + demand_misses)
miss reduction = (baseline_misses - dvr_misses) / baseline_misses
PC access share = PC demand accesses / committed demand memory instructions
```

### 全局 L1D 参考

按 gem5 的 L1D demand-access counter，Baseline 的整体 demand miss rate 为 13.33%，DVR run 为 8.79%。DVR run 的 overall miss rate 为 9.08%；其中 overall 还包括 helper 请求，因此不能直接当作主线程 demand miss rate。

## Cache miss 集中度：两个关键 PC 是否代表主要瓶颈？

这里需要区分两个概念：

- **PC miss rate**：某个 PC 的 misses / 该 PC 的 accesses；
- **miss concentration**：两个关键 PC 的 misses / 整个 L1D 的 misses。

“两个 PC 的 miss rate 加起来达到 95%”不是一个有意义的比例，因为两个 miss rate 的分母不同。对于判断 benchmark 是否由这条依赖链主导，应使用 miss concentration。

| 运行 | `0x103b4` misses | `0x103ba` misses | 两 PC 合计 | 全局 L1D demand misses | Miss concentration |
|---|---:|---:|---:|---:|---:|
| Baseline | 970 | 109 | 1079 | 1096 | **98.45%** |
| DVR demand | 150 | 33 | 183 | 790 | 23.16% |

Baseline 已经超过你要求的 95%，说明 Camel 的主要 cache-miss 瓶颈确实集中在这两个 PC。加入 DVR 后，这两个 PC 的主线程 demand misses 从 1079 降到 183：

```text
Demand miss reduction = (1079 - 183) / 1079 = 83.04%
```

DVR run 中还存在 helper 请求。将两个 PC 的 helper misses 一并计入：

```text
key-PC misses = 150 + 33 + 590 + 80 = 853
overall L1D misses = 870
key-PC overall miss concentration = 98.05%
```

因此，Baseline 证明 benchmark 的 miss 瓶颈由两个关键 PC 主导；DVR 结果证明这些 miss 被大量转化为 helper 预取流量，而不是简单消失。

### Helper 请求统计

| PC | DVR source hits | DVR source misses | DVR dependent hits | DVR dependent misses |
|---|---:|---:|---:|---:|
| `0x103b4` | 178 | 590 | 0 | 0 |
| `0x103ba` | 0 | 0 | 513 | 80 |

这些是 helper 请求在 cache 层的统计，不是主线程 demand miss。它们用于分析 helper 的带宽、MSHR 和 cache pollution 代价。

## 16/32/64/128 lane 回归

| Lanes | IPC | Helper decoded | Helper issued | Helper completed | Dependent generated | Dependent issued | Dependent completed | Reference mismatch | Predicate misses |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 2.1932 | 69 | 69 | 69 | 165 | 149 | 149 | 0 | 0 |
| 32 | 2.2008 | 120 | 120 | 120 | 287 | 287 | 287 | 0 | 0 |
| 64 | 2.2070 | 144 | 144 | 144 | 333 | 333 | 333 | 0 | 0 |
| 128 | 2.2171 | 288 | 288 | 288 | 593 | 593 | 593 | 0 | 0 |

16 lane 的 generated/issued 差异来自 bounded queue 的去重或背压，不是完成丢失；issued 与 completed 仍然严格相等。

## Reference 验证

在 128-lane Camel run 中：

| 指标 | 数量 |
|---|---:|
| Replay reference checks | 5496 |
| Address value matches | 5496 |
| Address value mismatches | 0 |
| Correct range extrapolations | 253 |
| Predicate diagnostics | 268 |
| Unavailable reference | 0 |

`minAddress/maxAddress` 和 stable mask 现在只作为诊断统计，不再阻止语义正确的 replay。非法地址和 MMU translation fault 仍然会被拒绝。

## 结果文件

PC 对照表：

```text
/home/lynnhoo/dvr-repro/results/camel-frozen-32k-baseline/cache_pc_system_cpu_dcache.csv
/home/lynnhoo/dvr-repro/results/camel-frozen-32k-dvr/cache_pc_system_cpu_dcache.csv
```

多 lane stats：

```text
/home/lynnhoo/dvr-repro/results/predicate-fix-camel-lanes-16/stats.txt
/home/lynnhoo/dvr-repro/results/predicate-fix-camel-lanes-32/stats.txt
/home/lynnhoo/dvr-repro/results/predicate-fix-camel-lanes-64/stats.txt
/home/lynnhoo/dvr-repro/results/predicate-fix-camel-lanes-128/stats.txt
```

## 复现实验

服务器进入固定构建环境后：

```bash
cd /home/lynnhoo/dvr-repro/source/gem5-dvr/code/gem5-runahead-dev-pre
export PYTHON_CONFIG=/tmp/python36-config
python3 -m SCons build/RISCV/gem5.opt -j32
```

单个 lane 配置的运行形式：

```bash
ROOT=/home/lynnhoo/dvr-repro/source/gem5-dvr/code/gem5-runahead-dev-pre
BENCH=/home/lynnhoo/dvr-repro/results/reference-split-camel-4096/camel.riscv
OUT=/home/lynnhoo/dvr-repro/results/camel-frozen-32k-dvr

DVR_PC_SUMMARY_DIR="$OUT" "$ROOT/build/RISCV/gem5.opt" \
  --outdir="$OUT" "$ROOT/configs/dvr/table1_se.py" \
  --cmd="$BENCH" --dvr --dvr-mode=full --dvr-vector-chunks \
  --dvr-max-lanes=128
```

## 当前限制

- Camel 是短输入，不能代表论文 500M-instruction ROI 的绝对性能。
- 当前表格关注功能正确性和 PC 级 cache 趋势，不应直接与论文的绝对 IPC 对齐。
- Kangaroo/GAP 的完整 ROI 和六组消融仍需单独完成。
