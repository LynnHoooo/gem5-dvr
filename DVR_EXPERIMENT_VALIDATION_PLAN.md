# DVR 实验验证、参数校准与 Workload 覆盖方案
我检查了服务器最新状态：

```text
分支：main
最新提交：4ab4b17
远端：origin/main
额外未提交修改：code/gem5-runahead-dev-pre/src/cpu/o3/cpu.cc
```

注意：最新实验二进制的 `manifest.txt` 仍显示基于 `784d428` 构建，因此还没有验证 `cpu.cc` 中的未提交修改。

| 模块 | 论文原文逻辑 | 服务器当前实现 | 当前判断 |
|---|---|---|---|
| RPT/Stride Detector | dispatch/execute 观察 stride load 和 stride 值 | `observeDVRDispatch()` 在 O3 dispatch 路径调用 stride detector | 基本一致 |
| Discovery 启动 | 发现合适 stride 后立即启动，不等待 ROB 满 | dispatch 阶段 `beginDVRDiscoveryAtDispatch()` 启动 | 一致 |
| Innermost stride | 每个 RPT entry 一个 bit；同一 stride PC 再次出现后切换到内层 stride | `discoverySeen`、重复检测、Discovery restart 已实现 | 基本一致 |
| Discovery 结束 | 主线程再次到达 trigger stride 后结束 Discovery | dispatch 记录边界，commit 阶段完成并生成 helper | 逻辑一致，但生成时序偏 commit |
| VTT | 每个架构寄存器一个 taint bit，沿依赖传播 | dispatch 记录 tainted instruction，并在 commit 验证 | 基本一致 |
| FLR | 最后一个 tainted load 的 PC 作为 FLR | committed dependent load 更新 FLR | 基本一致，但投机回滚仍简化 |
| Loop-Bound | 使用 LCR/SBB 找包围 stride 和 FLR 的 backward branch | 支持 `SLT/SLTU + BEQ/BNE`、`BNE`、`BLT/BGE/BLTU/BGEU` | 常见循环一致，复杂循环会 fallback |
| Vectorizer | 根据 stride 生成多个未来 iteration，并向量化 dependent chain | 最多生成 128 lanes，并产生 source/dependent 地址 | 功能一致 |
| VRAT | 标量寄存器可共享，tainted destination 分配 vector physical registers | helper-private VRAT、fresh 16-copy rename、physical lifetime 检查 | 结构一致，非 bit-exact |
| VIR | 16 个独立 vector copies，带 mask、顺序 issue | 已有 16-copy、active/ready/issued/completed/dead-source mask | 基本一致，但 scheduler 是自定义模型 |
| Helper frontend | 8-entry front-end buffer，产生 helper micro-ops | 独立 fetch/decode buffer、live RISC-V decode、`DVRDynUop` | 方向一致，仲裁细节近似 |
| Helper issue/FU | helper 与主线程共享 FU，主线程 ready uop 优先 | `tryIssueDVRHelperFU()` 使用共享 FU pool | 基本一致，但不是论文完整流水线 |
| Helper memory | helper load 进入真实 memory hierarchy | helper-local LQ，经过 MMU/cache/DRAM | 基本一致，但没有主 O3 DynInst/ROB/LSQ 生命周期 |
| Branch divergence | SIMT lane mask、alternate path、reconvergence | per-lane PC、mask、stack、same-PC grouping | 基础逻辑一致，Camel/BFS workload 覆盖不足 |
| Nested DVR | branch inversion → outer invocation → inner bound → flatten | 当前已实现并有 `flattened == expected` 检查 | 数据面成立，仍非 bit-exact NDM |
| 硬件开销 | 论文报告 1139 bytes | 当前没有按论文 bit-level 方式计算硬件开销 | 尚不能声称 1139 bytes |
| 实验配置 | Sniper、x86、AVX-512、5-wide、350 ROB、500M ROI | gem5、RISC-V、Camel/BFS、较小输入 | 不能直接复现论文绝对性能 |

### 服务器最新 Camel/BFS 结果

数据目录：

`/home/lynnhoo/dvr-repro/results/final/20260807T165300-combined-fixed`

| Workload | 模式 | 相对 Baseline IPC | 机制情况 | 正确性 |
|---|---:|---:|---|---|
| Camel | VR | 1.000x | 没有 helper activity | 通过，但没有收益 |
| Camel | Offload | 1.699x | 74513 helper 完成 | **无效：581 个 translation fault** |
| Camel | Discovery | 1.000x | 973 dependent target | 通过 |
| Camel | Multiple | 1.000x | flatten `115/115`，NDM 生效 | 通过，但没有收益 |
| BFS | VR | 1.000x | 128 source/dependent request | 通过 |
| BFS | Offload | 1.481x | 432367 helper 完成 | **无效：571 fault，committed 不守恒** |
| BFS | Discovery | 1.014x | 162 dependent target | **无效：1 fault，committed 不守恒** |
| BFS | Multiple | 1.000x | flatten `4174/4174`，NDM 生效 | 通过，但没有收益 |

因此当前可以确认：

- Camel/BFS 的 Discovery 和 Nested 数据路径已经真正运行；
- `Multiple` 的 flatten 不是零；
- helper issue/completion 和 dependent request 守恒正常；
- 但 Camel/BFS 目前没有证明性能提升；
- Offload 的高 speedup 不能采用，因为伴随 translation fault；
- BFS/Camel 的 alternate path 仍未产生有效 dependent coverage。

目前最大的剩余差异是：

1. 复杂 loop-bound 和 early-exit；
2. BFS/Camel 的 alternate-path 完整执行；
3. helper evaluator 与统一前端/VIR/内存流水线的完全统一；
4. translation fault 和 committed mismatch；
5. 论文原始 workload、输入规模和 500M ROI 尚未对齐；
6. `4ab4b17` 之后的未提交 `cpu.cc` 修改还没有重新编译验证。
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

## 5.1 PC 级 miss -> DVR pipeline 验证（2026-08-08）

配置澄清：DVR logical lane 上限是 `128`，64-bit element 下每个 512-bit FU chunk
执行 `8` lanes，VIR 的 `16` copies 合计 `16 * 8 = 128` scalar-equivalent lanes。
最初的 `camel-pc-pipeline-smoke2-20260808` 漏传 `--dvr-vector-chunks`，其 PC/cache
因果统计仍有效，但性能不作为受限 vector-FU 结果。修正后的目录是：

```text
/home/lynnhoo/dvr-repro/results/camel-pc-pipeline-chunked-20260808
```

该配置确认 `dvrMaxLanes=128`、`dvrVectorChunkModel=true`、element width `64`。
实际 `dvrTotalActiveLanes / dvrLaneCountSamples = 131249 / 1034 = 126.93`
lanes/launch。本输入的 `dvrNestedFlattenedLanes=0`，所以这些是普通 Discovery helper
lanes，不是 NDM outer-invocation flatten lanes。

在继续追 Figure 8 趋势前，先验证单条访存指令的完整因果链。cache tag lookup
现已在 `BaseCache::recvTimingReq()` 的真实 `satisfied` 判定点按 request PC 聚合，分别记录：

```text
architectural demand hit/miss
DVR source hit/miss
DVR dependent hit/miss
L2/LLC demand miss
```

该统计不是用 load latency 猜测 miss，也不会逐动态指令写日志。设置
`DVR_PC_SUMMARY_DIR` 后，每个 cache 只输出一行/PC。`summarize_camel_dvr_trace.py`
将 cache 结果与 trigger、tainted、FLR、relation、replay target、fault 合并为：

```text
pc_pipeline_summary.csv
```

Table-1 配置、Camel `MAX_KEY=4096`、Nested 模式的首轮 smoke 证据位于：

```text
/home/lynnhoo/dvr-repro/results/camel-pc-pipeline-smoke2-20260808
```

反汇编与统计对应如下：

| PC | Camel 语义 | L1D demand hit/miss | DVR request hit/miss | DVR pipeline |
|---|---|---:|---:|---|
| `0x103aa` | `ld a4,0(s1)`，即 `array2[i]` source | `4092 / 5` | source `10307 / 522` | trigger events `95960` |
| `0x103b0` | `lw a4,0(a4)`，即 `*array2[i]` dependent FLR | `4084 / 12` | dependent `3911 / 101` | tainted/FLR `985/985`，relation `1038`，target `4012` |

总体机制统计：

```text
vectorProgramsBuilt       = 331
replayTargetsGenerated    = 4012
dependentDemandCovered    = 3822
translationFaults         = 0
```

L1D PC 聚合之和与 cache 原生统计严格相等：demand `24144 hit / 14200 miss`、
source `10307 / 522`、dependent `3911 / 101`。因此 PC 标注和 cache 分类已通过
守恒检查。`0x1038a` 等初始化写产生大量 miss，但不属于 pointer-chain ROI；后续性能
解释必须把初始化/libc PC 与 `0x103aa -> 0x103b0` 链分开，不能使用全程序 miss 总数
直接归因给 DVR。

同一 ELF、同一 Table-1 配置的 Baseline 对照目录为：

```text
/home/lynnhoo/dvr-repro/results/camel-pc-baseline-smoke-20260808
```

| PC | Baseline L1D demand hit/miss | Nested L1D demand hit/miss | 可验证结论 |
|---|---:|---:|---|
| `0x103aa` source | `3129 / 968` | `4092 / 5` | source prefetch 使 stride load 的需求 miss 大幅下降 |
| `0x103b0` dependent | `3984 / 112` | `4084 / 12` | dependent replay/prefetch 消除了 `100/112 = 89.3%` 的关键 demand miss |

同时，Baseline/Nested 的 `simTicks` 是 `154212500 / 149548500`，IPC 是
`0.839052 / 0.865220`。这不是完整论文规模的性能结论，但已证明该 Camel 实例中
性能变化和关键 pointer-chain miss 的消除方向一致。

全程序 L1D demand miss 从 `15263` 降到 `14200`，恰好减少 `1063`；source PC
减少 `968 - 5 = 963`，dependent PC 减少 `112 - 12 = 100`，两者之和同样是
`1063`。因此本轮所有 demand-miss 改善都能归因到这两条 pointer-chain 指令，而不是
其他 PC 的偶然 cache 行为。

层级化 cache 结果保存于 `cache_level_summary.csv`。这里必须区分 local miss rate 与
到达下层的绝对请求数：

| 层级 | Baseline demand access/miss/rate | Nested demand access/miss/rate |
|---|---:|---:|
| L1D | `38346 / 15263 / 39.80%` | `38344 / 14200 / 37.03%` |
| L2 | `3072 / 2344 / 76.30%` | `2832 / 2344 / 82.77%` |
| L3 | `2343 / 2343 / 100%` | `2343 / 2343 / 100%` |

Nested 的 L2 local miss rate 上升不是更多 miss，而是到达 L2 的 demand 从 `3072`
下降到 `2832`，但本轮冷启动/容量 miss `2344` 不变。DVR 到达 L2 的 source 请求
`141/141` hit，dependent 请求 `101/101` hit，均未进入 L3。关键 `0x103aa` 和
`0x103b0` 的 demand L1 miss 也全部在 L2 hit。因此这个短输入证明的是 DVR 将关键链
提前放入 L1D；它没有降低 LLC/内存 miss，不能用来声称 DRAM MLP 已提升。

下一轮固定相同 binary、cache/core 参数和 ROI，对 Baseline、Discovery、Nested 输出：

```text
关键 PC 的 demand miss 变化
source/dependent request 的 L1/L2 去向
replay target -> completed -> covered 比率
late/pollution 与 simTicks/IPC
```

只有上述链路守恒且 Nested 的额外请求来自真实 outer invocation，才进入论文规模的
Figure 8 消融。

## 5.2 固定输入的 Camel hot-PC cache sweep（2026-08-08）

正式结果使用统一输入 `MAX_KEY=65536`。PC cache 聚合器现已连接 gem5 statistics
reset/dump callback：`m5_reset_stats()` 清空 PC map，`m5_dump_stats()` 写出并冻结。
脚本强制检查每个 cache PC CSV 的 hit/miss 合计必须等于第一段 ROI stats；四组均通过。
因此以下结果不含 reset 前初始化访问，也不含 dump 后 printf/退出访问：

```text
/home/lynnhoo/dvr-repro/results/camel-hot-pc-cache-sweep-max65536-20260808
```

| L1D | 模式 | L1D accesses | all misses/rate | hot misses/L1D accesses | hot/all misses | IPC | DVR speedup |
|---|---|---:|---:|---:|---:|---:|---:|
| 16 KiB | Baseline | 131103 | 77292 / 58.96% | 77278 / 58.94% | 99.98% | 1.0088 | 1.000x |
| 16 KiB | DVR | 131102 | 40670 / 31.02% | 40656 / 31.01% | 99.97% | 1.5313 | 1.518x |
| 32 KiB | Baseline | 131103 | 73836 / 56.32% | 73822 / 56.31% | 99.98% | 1.0179 | 1.000x |
| 32 KiB | DVR | 131102 | 27650 / 21.09% | 27635 / 21.08% | 99.95% | 1.6108 | 1.582x |

四组 committed instructions 均为 `5767296`、程序输出均为 `Result 2125659619`、
translation fault 均为 0。这里约 99.9% 的含义严格是 `hot misses / all misses`；
`all misses / L1D accesses` 是 21.09%--58.96%，两者不可互换。

以下 `MAX_KEY=4096` 表格仅保留为短输入 smoke，不用于正式性能结论。

Camel 使用同一个 `MAX_KEY=4096` ELF，并启用源码已有的 `m5_reset_stats()` /
`m5_dump_stats()`，排除初始化和退出代码。扫描中仅改变 L1D capacity；associativity、
latency、MSHR、L2/L3、stride prefetcher、128-lane DVR 和输入全部不变。结果目录：

```text
/home/lynnhoo/dvr-repro/results/camel-hot-pc-cache-sweep-20260808
```

由于 m5ops 改变了 ELF 布局，本 binary 的 source/dependent PC 分别是 `0x103b4` 和
`0x103ba`。完整结果在 `camel_hot_pc_cache_sweep.csv`：

| L1D | 模式 | ROI L1D accesses | ROI misses | hot misses / L1D accesses | source miss rate | dependent miss rate | IPC |
|---|---|---:|---:|---:|---:|---:|---:|
| 4 KiB | Baseline | 8223 | 3609 | 43.69% | 9.30% | 78.42% | 1.5789 |
| 4 KiB | DVR | 8223 | 3557 | 43.06% | 0.05% | 86.40% | 1.5794 |
| 8 KiB | Baseline | 8223 | 2884 | 34.88% | 11.76% | 58.25% | 1.6738 |
| 8 KiB | DVR | 8223 | 2661 | 32.16% | 0.05% | 64.53% | 1.7040 |
| 16 KiB | Baseline | 8223 | 1621 | 19.52% | 16.01% | 23.17% | 1.8695 |
| 16 KiB | DVR | 8223 | 574 | 6.79% | 0.05% | 13.57% | 2.0845 |
| 32 KiB | Baseline | 8222 | 1095 | 13.12% | 23.68% | 2.66% | 1.9947 |
| 32 KiB | DVR | 8223 | 31 | 0.18% | 0.07% | 0.29% | 2.2898 |
| 64 KiB | Baseline/DVR | 8223 | 3 | 0% | 0% | 0% | 2.2971 |

更正：上表的 `hot misses / L1D accesses` 才是 cache-miss rate 口径。当前扫描没有
任何一点达到 90%，最高是 4 KiB Baseline 的 43.69%。先前的 99% 是 `hot misses /
all misses`，不能作为 miss rate。16 KiB 仍是机制验证的平衡点：DVR 将 hot misses 从
`1605` 降到 `558`，ROI misses 从 `1621` 降到 `574`，IPC speedup 为
`2.084530 / 1.869498 = 1.115x`，程序输出均为 `Result 8006810`，translation fault 为 0。

16 KiB 下 Baseline/DVR 的 L3 demand miss 都是 6。DVR 到达 L2 的 source `308`
和 dependent `484` 请求全部 L2 hit，说明收益来自 L2-to-L1D timeliness，而非减少
LLC/DRAM 工作量。当前 128-lane DVR 仍产生 `127250` 个 source L1D request，流量偏高，
后续需要单独报告请求去重、late ratio 和 traffic overhead。

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
