# 当前工作盘点

## 1. 当前研究主题

利用异构移动 SoC 中的空闲小核，提前执行不规则访存的 load slice，为大核生成跨核预取并提高 MLP。

## 2. 已形成的工作

| 部分 | 当前内容 | 状态 |
|---|---|---|
| 核心机制 | LeAP：硬件动态检测并提取 load slice | 已形成完整设计 |
| 并行方式 | 跨多个小核展开迭代，并在单个小核内流水执行 | 已形成完整设计 |
| 数据传递 | 基于 CHI Stash 扩展 DFM，将数据送入大核 L1 | 已形成完整设计 |
| RTL 实现 | Chipyard/RocketChip + BOOM/Rocket，LeAP 使用 Chisel 实现 | 已有代码 |
| 评估平台 | gem5、RISC-V 异构核、Ruby CHI | 已完成论文级评估 |
| 工作负载 | Graph500、HashJoin、Camel、Kangaroo、NAS、RandomAccess | 已完成评估 |
| 硬件开销 | SEU、PDC、SDU 的 RTL 综合估算 | 已完成 |
| 论文调研 | 预取、helper thread、runahead、DAE、移动异构与图计算 | 已分类整理 |

## 3. LeAP 的已有结论

| 指标 | `final_version.pdf` 中的结果 |
|---|---:|
| LeAP-512 几何平均加速 | 2.48x |
| IMP 几何平均加速 | 1.15x |
| 多线程执行几何平均加速 | 1.35x |
| 平均预取准确率 | 89% |
| 平均预取覆盖率 | 93% |
| 面积开销 | 0.33% |
| 功耗开销 | 0.28% |

这些结果支持：执行式跨核预取能够提高不规则负载的 MLP，并且直接把数据送到大核私有缓存具有明显价值。

## 4. 当前代码实现

远端仓库：`/home/lynnhoo/LeAP`。只读检查确认了以下代码：

| 位置 | 已实现内容 |
|---|---|
| `Hardware/leap` | StrideDetector、DependencyTracker、SEU、SDU、PDC、VAG |
| `Hardware/boom` | 大核接入、LSU/DCache 和 MSHR 修改 |
| `Hardware/rocket-chip` | 小核预取执行、自定义 pf 指令、L1Push |
| `Hardware/rocket-chip-inclusive-cache` | LeAP 定制通道、grant data 镜像和 push acknowledgement |
| `Benchmark/leap-bench` | Camel、HashJoin、GAP、NAS、Kangaroo 和 bare-metal 支持 |

最近一轮提交主要完成：

- `L1Push/leappush` 数据推送路径；
- inclusive cache 到 BOOM L1 DCache 的定制通道；
- 自定义 pf 指令和硬件依赖链构建；
- BOOM LSU/MSHR 冲突修复；
- GAP 的 BC、BFS、CC、PageRank、SSSP 等负载接入。

代码能够证明机制已落到 RTL，但仍需用回归测试区分“已实现”和“已稳定验证”。远端工作区还存在未提交修改，本文档未改动这些文件。

## 5. ARM HMP 验证平台

已在 `mobile-hmp-gem5` 中准备：

- `1 x X2-like + 3 x A78-like + 4 x A55-like` 参数化 ARM HMP 配置；
- Ruby CHI 私有 L1/L2、共享 HNF/L3 配置；
- `none/BOP/SPP/SPPv2` 核内预取基线；
- microbenchmark、graph benchmark、启动与结果解析脚本；
- CHI 数据来源统计和 SPPv2 有界 lookahead 补丁。

当前材料尚不能确认：

- ARM64 八核 full-system 已稳定启动；
- PageRank、CC、BFS 已完成完整实验；
- 跨核预取机制已迁移到新 ARM 平台；
- 性能、CHI 流量和能耗结果已经产出。

## 6. 已完成的研究准备

- 明确了移动端访存瓶颈、空闲小核和跨核协作的 motivation；
- 整理了 Slipstream、DCE、Ghost Threading、Speculative Precomputation、Inter-Core Prefetching 等机制；
- 设计了平台验证、访存瓶颈确认、理想上限和真实图负载等预实验；
- 明确新平台中的核心指标：性能、accuracy、coverage、timeliness、CHI 流量、cache pollution 和能效。

## 7. 当前最需要回答的问题

> 新工作是在更真实的 ARM HMP/CHI 平台上验证 LeAP，还是基于已有 LeAP 重新提出一种更轻量、更通用的跨核预执行机制？

在确定故事线前，应先完成最小验证：ARM64 启动、单个图负载基线，以及一次可观测的数据跨核传递。之后再决定保留、修改或重做哪些实验。

## 8. 证据边界

- 已有 LeAP 结果来自参数化 RISC-V/gem5 平台，不能直接表述为真实 Arm SoC 结果。
- Chipyard/Chisel RTL 实现增强了机制可信度，但不等同于流片或真实手机验证。
- X2-like、A78-like、A55-like 是研究模型，不能冒充对应 Arm RTL。
- 当前 ARM HMP 目录证明了配置与实验工具已经准备，不等同于平台和实验已经验证完成。
