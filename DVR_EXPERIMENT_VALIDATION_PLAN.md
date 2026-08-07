# DVR 实验验证、参数校准与 Workload 覆盖方案

## 1. 目标与原则

实验分成“机制验收、论文配置锚定、参数校准、workload 扩展、统一平台比较”五层。
任一层失败时停止扩大 workload，先修复该层暴露的问题。所有模式使用相同 ELF、输入、
ROI、cache/DRAM 配置和 committed-instruction gate；DVR 请求必须进入真实 MMU、cache、
MSHR 和 DRAM contention。

固定比较模式：

```text
baseline, vr, offload, discovery, full, nested, oracle
```

`full` 表示 Offload + Discovery；`nested` 才包含 NDM。不能把 `full` 的结果标成完整
Nested DVR，也不能把 `oracle` 标成真实 DVR。

## 2. 第一层：机制验收

使用已有脚本和专项 workload：

```bash
bash scripts/run_remote_dvr_stage16_algorithm_smoke.sh
bash scripts/run_remote_dvr_helper_regression.sh
bash scripts/run_remote_dvr_lbd_vtt_regression.sh
bash scripts/run_remote_dvr_alternate_path.sh
bash scripts/run_remote_dvr_ndm_e2e.sh
```

必须满足：

```text
baseline committed == DVR committed
helper decoded == helper issued == helper completed
load allocated == completed + faults + retries + dropped + pending
VIR active-mask failures == 0
translation faults == 0（有效专项地址）
reconvergence stack overflows == 0
flattened lanes == min(128, sum(each invocation inner lanes))
```

M1：单层 `A[B[i]]` coverage > 80%，平均 lanes > 1，dependent targets > 0。

M2：短 inner-loop workload 必须有 `outer_instances >= 2`、`nested_batches > 0`、
`flattened_lanes > 0` 和 nested helper completion。

M3：divergent workload 必须有 alternate uop、dependent target、demand coverage 和
reconvergence resume；BFS/BC 的 active-lane 丢失原因必须可由 early exit 或 predicate 解释。

## 3. 第二层：论文配置锚定

建立单独配置，尽量匹配论文 Table 1：5-wide O3、350-entry ROB、32 KiB L1D、24 MSHR、
256 KiB L2、8 MiB LLC、相同 DRAM timing，并保持硬件 stride prefetcher 开启。首轮使用
较短 ROI 验证，最终报告使用统一 warm-up 和 500M ROI instructions，无法达到 500M 的
workload 必须报告完整程序指令数。

首批 workload：

```text
dvr_single_indirect
dvr_lbd_vtt
dvr_nested / variable-bound nested
NAS-IS
GAP BFS、BC、PageRank
Camel
```

运行入口：

```bash
bash scripts/run_remote_dvr_anchor.sh
bash scripts/run_remote_dvr_figure8.sh
```

通过线：至少三个 workload 的 speedup 方向与论文一致；简单间接 workload 必须稳定正收益；
DVR 平均 outstanding misses 明显高于 baseline，目标为 baseline < 4、DVR > 10。若达不到，
必须同时报告 helper queue、MSHR、FU stall、late prefetch 和 DRAM bandwidth 诊断。

## 4. 第三层：参数校准

只使用 `dvr_single_indirect`、NAS-IS、BFS、PageRank 和 Camel 校准，禁止针对最终完整
workload 集逐项调参。采用分阶段网格搜索：

| 参数 | 搜索值 |
|---|---|
| max lanes | 32, 64, 128 |
| NDM threshold | 32, 64, 96 |
| helper max uops | 100, 200, 400 |
| vector issue interval | 1, 2, 4 cycles |
| helper LQ capacity | 8, 16, 24 |
| frontend arbitration | main-priority, round-robin, unlimited sensitivity |
| vector FU | constrained, unlimited sensitivity |

默认选择不是最高 speedup，而是满足以下条件后几何平均 speedup 最大的配置：

```text
accuracy >= 70%
coverage >= 20%（简单间接 workload >= 80%）
translation faults == 0
pollution 不高于 baseline demand eviction 的 10%
helper traffic 不超过 baseline DRAM bytes 的 2x
```

保存 constrained/unlimited FU 两组结果，用两者差值解释资源模型敏感性。固定参数后生成
`calibration.json`，后续 workload 不再修改。

## 5. 第四层：Workload 覆盖

按能力分组，避免只报告有收益程序：

| 能力 | Workload |
|---|---|
| 简单间接链 | NAS-IS、RandomAccess、Camel |
| 图与分支 | GAP BFS、BC、CC、PR、SSSP |
| 两级间接 | NAS-CG、Graph500、HJ2/HJ8 |
| 短内循环/NDM | variable-bound nested、Kangaroo、短邻接表图 |

GAP 五 workload 使用：

```bash
bash scripts/run_remote_dvr_gap5_ablation.sh
```

每个 workload 至少报告：cycles、IPC、speedup、trigger、Discovery success、loop-bound
match/fallback、平均 lanes、source/dependent requests、useful/late/evicted、accuracy、coverage、
timeliness、MSHR occupancy、outstanding misses、DRAM bytes、pollution、FU/LSQ/frontend stalls。

M4：useful、late、evicted 和 pollution 均来自真实 cache event，不允许用 issued request
数量代替。

M5：论文配置下至少三个 workload 趋势一致；差距超过 20% 时必须给出模拟器、ISA、编译器、
输入或内存模型解释。

M6：冻结参数后迁移到统一平台，DVR 和待比较设计共享全部 core/cache/DRAM 参数及输入。

## 6. Camel/BFS 统一消融实测（2026-08-07）

已使用 `/home/lynnhoo/gem-test/gem5-leap/leap-bench` 的 Camel 与 GAP BFS，在同一
gem5 配置和 ROI 下完成 Baseline、VR、Offload、Discovery、Multiple/NDM、Oracle 六组。
合并证据目录：

```text
/home/lynnhoo/dvr-repro/results/final/20260807T165300-combined-fixed
```

当前实测结果不能直接宣称论文 Figure 8 趋势已复现：Offload 的 IPC 提升伴随真实
translation faults；Discovery/Multiple 产生 helper/dependent activity，但 Multiple 在这两个
输入上没有 IPC 提升，部分 BFS/Camel DVR 行的 committed 数量也未与 baseline 守恒。
这些行在 `correctness.csv` 中保留为 `observe`，没有被改写成通过。

Camel 资源敏感性目录：

```text
/home/lynnhoo/dvr-repro/results/final/20260807T165000-resource
```

已覆盖 MaxUops 8/16/32/64、vector FU constrained/unlimited、element width 32/64 和
max lanes 32/64/128；VIR copies 与 helper frontend width 当前没有参数入口，NDM 外层上限
由实现固定为 16，因此只记录为未暴露/固定配置。

## 7. 结果归档与判定

每次运行目录必须包含：

```text
git_commit.txt
gem5.opt sha256
benchmark ELF sha256
command.txt
config.ini
config.json
stats.txt
stdout.log / stderr.log
ROI、输入和编译器版本
summary.csv
```

最终输出三张核心表：Figure 8 消融、性能/MLP、accuracy/coverage/timeliness/pollution。
只有 M1-M5 全部通过后，论文配置结果才可称为 DVR reproduction；M2 未通过时标为
`DVR-Offload+Discovery`，使用标注或离线 bound 时标为 `Oracle-assisted DVR`。
