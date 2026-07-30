# 移动端跨核预取：预实验设计

## 1. 预实验目标

本阶段不追求完整论文结果，而是用最小实验集回答以下问题：

1. PageRank、CC、BFS 等图负载在 X2-like 大核上是否确实受访存限制？
2. 目标核被内存阻塞时，A55-like 小核是否具有可利用的执行与功耗余量？
3. 在 Ruby CHI 层次中，小核发出的访问能否在大核需求到达前，将数据放入有用的位置？
4. 跨核预取的收益是否超过核间通信、一致性流量、带宽竞争和辅助核能耗？
5. 跨核方案相对 BOP、SPP/SPPv2 等核内预取器是否具有独立价值？

预实验只支持“机制值得继续研究”或“当前设计需要调整”，不用于直接宣称真实 Arm 核或真实手机上的收益。

## 2. 研究假设与通过条件

| 编号 | 假设 | 主要证据 | 建议通过条件 |
|---|---|---|---|
| H1 | 图负载存在显著访存瓶颈 | MPKI、memory-stall cycles、MLP、带宽敏感性 | 至少两个核心负载的内存阻塞周期占比 ≥ 30%，且增大 LLC/降低内存延迟可明显加速 |
| H2 | 小核能够提前生成目标地址 | helper IPC、地址生成吞吐、预取覆盖率 | helper 能覆盖 ≥ 20% 的基线 demand misses |
| H3 | 跨核预取具有及时性 | useful/timely/late prefetch | useful 且 timely 的预取占发出预取的比例达到可解释水平，late 比例可通过预取距离调节 |
| H4 | CHI 通信代价可控 | RNF/HNF 消息、snoop、owner transfer、额外流量 | 性能提升不是由无限带宽假设获得；额外 DRAM 流量与 speedup 成比例且无明显拥塞反转 |
| H5 | 跨核设计有独立价值 | 与 none、BOP、SPP/SPPv2 比较 | 在至少一类间接/不规则负载上优于等预算核内预取，或以更低目标核开销取得相近收益 |
| H6 | 系统能效可能改善 | 全系统 energy、EDP、辅助核能耗 | 至少代表性配置的 EDP 改善；若只改善性能但增加能量，需明确适用场景 |

表中的阈值是预实验筛选线，不是最终论文结论。正式实验应报告完整分布、置信区间和负结果。

## 3. 基础平台

### 3.1 体系结构配置

| 模块 | 初始配置 | 预实验变量 |
|---|---|---|
| CPU | 1× X2-like、3× A78-like、4× A55-like | 首轮仅使用 1× X2-like 目标核和 1× A55-like helper，稳定后扩展 |
| ISA/系统 | ARM64 Linux full system | 固定 kernel、rootfs、编译器和启动参数 |
| 一致性 | Ruby CHI | 记录 RNF/HNF 请求、snoop、数据来源和状态迁移 |
| Cache | 每核私有 L1/L2，共享 HNF/L3 | L2/LLC 容量、放置位置、inclusive/exclusive 属性按实际模型记录 |
| Memory | LPDDR5-like | 延迟、通道数、峰值带宽、队列深度 |
| DVFS | X2/A78/A55 三个独立域 | 首轮固定频率；机制稳定后扫描频率组合 |
| 功耗 | gem5 stats + 与模型匹配的功耗估计 | 动态与静态能量分开，明确哪些结构为估算值 |

X2-like、A78-like、A55-like 仅表示参数化性能层级。所有结果必须报告具体 pipeline、ROB、LSQ、MSHR、cache、频率等参数，不能把它们表述为真实 Arm RTL 的精确复现。

### 3.2 首轮固定条件

为了避免 DVFS、热状态和调度干扰，首轮实验统一固定：

- CPU 与内存频率；
- 核绑定和内存分配策略；
- cache、MSHR、内存控制器及 CHI 参数；
- Linux kernel、rootfs、编译器和 benchmark commit；
- 输入图、线程数、ROI 起止位置及 warm-up 方法；
- 不运行无关后台任务。

## 4. 工作负载设计

### 4.1 核心负载

| 算法 | 访存特征 | 预期作用 |
|---|---|---|
| BFS | frontier 驱动、间接访问、阶段性明显 | 观察不规则访问、低局部性和 phase 变化 |
| CC | 多轮迭代、间接更新和共享数据 | 观察收敛阶段、共享与潜在 false sharing |
| PageRank | 重复遍历边和顶点属性 | 观察稳定迭代及相对可重复的访问模式 |

建议增加两个参照负载：一个顺序流式 microbenchmark，用于验证跨核搬运的理想上限；一个随机 pointer-chasing microbenchmark，用于验证间接地址生成能力和最坏延迟。

### 4.2 输入图

每个算法至少选择两种结构差异明显的图：

| 图类型 | 特征 | 研究价值 |
|---|---|---|
| Uniform Random | 度分布相对均匀 | 降低热点影响，观察平均行为 |
| Kronecker/R-MAT | 幂律、高度数热点 | 观察不均衡、cache 热点和并发请求 |
| 真实图（如 LiveJournal） | 社区结构和真实度分布 | 验证合成图结论能否迁移 |

规模分为三档：

- Small：数据可大部分进入 LLC，用于功能验证；
- Medium：工作集略大于 LLC，用于快速参数扫描；
- Large：明显超过 LLC，用于验证 DRAM 与带宽瓶颈。

必须同时报告顶点数、边数、内存占用和“工作集/LLC 容量比”，而不是只报告数据集名称。

## 5. 对照组

### 5.1 必需配置

| 配置 | 含义 | 目的 |
|---|---|---|
| `none` | 关闭数据预取 | 基准性能与流量 |
| `BOP` | 核内 BOP | 规则/offset 型预取基线 |
| `SPP` | 核内 SPP | signature/path 型基线 |
| `SPPv2` | 核内 SPPv2 | 更强的原生预取基线 |
| `helper-only` | helper 执行地址生成，但不产生有效预取 | 分离线程执行和同步开销 |
| `cross-core` | A55-like helper 为 X2-like 目标核预取 | 核心方案 |
| `oracle` | 使用未来 demand trace 或零计算代价生成预取 | 给出 coverage/timeliness 上限，而非可实现方案 |

### 5.2 公平性约束

不同预取器应尽可能统一：

- 预取插入层级和最终数据落点；
- 最大在途请求数与 MSHR 占用；
- 每周期可发出请求数；
- 允许使用的内存带宽；
- metadata/hardware storage 预算；
- cache replacement priority 和 pollution 控制；
- warm-up、ROI 和统计口径。

如果某个原生预取器无法满足相同接口，应同时报告“默认配置”和“等资源配置”，避免只比较经过充分调优的跨核方案与默认基线。

## 6. 分阶段预实验

### 阶段 A：ARM64 + Ruby CHI 平台验证

**目标：**确认八核启动、cache 层次、CHI 一致性和统计数据可信。

| 测试 | 检查内容 | 通过标准 |
|---|---|---|
| 单核读写 | L1/L2/LLC/DRAM 命中路径 | 延迟和消息路径符合配置 |
| 跨核共享读 | shared line 的获取与复用 | RNF/HNF 消息及数据来源可解释 |
| producer–consumer | dirty owner 数据转移 | 正确识别 owner transfer，不误记为 DRAM miss |
| false-sharing ping-pong | cache line 反复失效 | snoop/invalidations 数量随迭代增长 |
| STREAM-like | 内存带宽上限 | 带宽不超过配置峰值，通道利用率合理 |

在本阶段通过前，不进行图负载性能比较。

### 阶段 B：证明图负载受内存限制

只运行 X2-like 目标核，关闭所有预取。每个 workload 收集：

- IPC/CPI、L1/L2/LLC MPKI；
- memory-stall cycles 及其占比；
- outstanding misses/MLP 分布；
- DRAM row hit、平均延迟、队列占用和带宽；
- 指令、branch、TLB miss；
- 各迭代或固定时间窗口的 phase 数据。

进行三组敏感性实验：

| 扫描项 | 建议档位 | 用途 |
|---|---|---|
| 内存延迟 | 0.5×、1×、1.5× | 若降低延迟明显加速，说明 latency-bound |
| 内存带宽 | 0.5×、1×、2× | 区分 bandwidth-bound 与 latency-bound |
| LLC 容量 | 0.5×、1×、2× | 判断容量缺失是否为主因 |

如果负载对上述变量均不敏感，应先检查输入规模、ROI、CPU 参数和并行实现，而不是直接进入预取实验。

### 阶段 C：测量跨核预取的理想上限

使用 oracle 地址流，分别将预取数据放入：

1. 目标核私有 L2；
2. 共享 LLC；
3. 仅提前进入内存控制器/缓存队列。

扫描 prefetch distance、在途请求数和注入速率。该阶段回答：在不考虑 helper 地址生成成本时，CHI 层次中哪个落点和提前量最有效。

如果 oracle 也无法获得明显收益，说明瓶颈可能不是可预取的 cache miss，或当前 cache/CHI 插入机制无效，应停止开发完整 helper。

### 阶段 D：最小跨核 helper

首个实现只使用 1× X2-like + 1× A55-like：

- 目标核执行原始图算法；
- helper 根据 frontier/offset/edge 等必要状态生成地址；
- 两核通过明确的队列或共享控制结构传递任务；
- 预取请求使用独立标记，以便在 CHI、cache 和 DRAM 全路径追踪；
- 分别测量 helper 的计算时间、等待时间、同步时间和发出请求时间。

先选择 PageRank 或稳定迭代的 BFS phase 完成功能验证，再扩展到 CC 和完整 BFS。

### 阶段 E：基线比较与参数扫描

推荐先用 Medium 图执行下列最小矩阵：

| 维度 | 档位 |
|---|---|
| Workload | PageRank、CC、BFS |
| 图结构 | Uniform、R-MAT/真实图 |
| Prefetcher | none、BOP、SPP、SPPv2、cross-core、oracle |
| Prefetch distance | 近、中、远三个档位 |
| 落点 | target L2、shared LLC |
| Helper 数量 | 1；有明确收益后再测试 2/4 |

找到稳定配置后，再加入 Large 图、DVFS、MSHR、内存带宽和多 helper 扫描。不要在机制尚未稳定时做全笛卡尔积。

### 阶段 F：DVFS 与能效

固定目标核与 helper 的核绑定，扫描：

| X2-like | A55-like | 目的 |
|---|---|---|
| 高 | 低/中/高 | 找到 helper 的最低有效频率 |
| 中 | 低/中/高 | 观察目标核变慢后及时性变化 |
| 高 | 关闭 | none/核内预取的能量基线 |

至少报告 runtime、energy、EDP，并拆分目标核、helper、cache/NoC、DRAM 的能量。若功耗模型不能准确覆盖 CHI 或自定义队列，应把对应结果标记为估算，并单独报告活动量。

## 7. CHI 插桩与事件分类

每个 demand miss 和 prefetch 应携带可关联的唯一标识，至少记录请求核、目标核、地址、发出时间、完成时间、数据来源、CHI 事务类型、cache 状态变化及最终用途。

### 7.1 数据来源分类

| 类别 | 定义 | 需要区分的原因 |
|---|---|---|
| Private-cache hit | 数据已在请求核私有 cache | 不应计为跨核或 DRAM 收益 |
| Remote shared clean | 数据来自其他核的 clean/shared 副本 | 衡量远程共享与 snoop 开销 |
| Dirty-owner transfer | 数据由持有 dirty line 的核提供 | 延迟和消息数不同于普通共享 |
| LLC/HNF hit | 数据由共享 LLC/HNF 提供 | 衡量预取是否成功进入共享层 |
| DRAM miss | 最终访问内存 | 衡量带宽、延迟和 row-buffer 行为 |
| Migration/cold miss | 线程或数据迁移后首次访问 | 避免把迁移效应误判为预取收益 |
| False sharing | 不同核写同一 line 的不同字段 | 识别协议流量而非有效数据共享 |

### 7.2 预取结果分类

| 类别 | 定义 |
|---|---|
| Useful-timely | demand 到达前完成，并被目标 demand 使用 |
| Useful-late | demand 已发出后预取才完成，但可能合并请求或部分降低延迟 |
| Too-early | 在使用前被替换或失效 |
| Redundant | 发出时数据已存在、已有同地址请求或被其他预取覆盖 |
| Harmful | 引起有用 line 淘汰、额外一致性冲突或关键 demand 延迟 |
| Unused | ROI 结束前未被 demand 使用 |

建议计算：

\[
Accuracy = \frac{UsefulPrefetches}{IssuedPrefetches}
\]

\[
Coverage = \frac{DemandMissesEliminated}{BaselineDemandMisses}
\]

\[
Lateness = \frac{UsefulLate}{UsefulTimely + UsefulLate}
\]

其中“消除的 demand miss”必须相对 none 基线按相同 ROI 统计，不能简单使用启用预取后的 miss 数作为分母。

## 8. 主要指标

| 类别 | 指标 |
|---|---|
| 性能 | ROI runtime、cycles、IPC、speedup、每轮迭代时间 |
| Cache/TLB | 各级 MPKI、hit/miss、replacement、prefetch pollution、TLB MPKI |
| 访存并行度 | outstanding misses、MSHR occupancy、MLP 分布、平均 miss latency |
| 预取质量 | issued、useful、timely、late、too-early、redundant、unused、accuracy、coverage |
| CHI/NoC | 各类 request/response/snoop、flit/byte 数、hop、queue latency、owner transfer |
| DRAM | read/write bytes、带宽利用率、row hit、queue occupancy、平均/尾延迟 |
| Helper | IPC、活跃周期、等待周期、地址生成吞吐、任务队列占用、同步开销 |
| 能效 | 总 energy、分组件 energy、平均功率、EDP/ED²P |
| 成本 | 队列、metadata、额外 cache 状态、表项数和估算存储字节数 |

所有 speedup 使用 `none` 的同工作负载、同输入、同 ROI 作为分母；同时给出相对最强核内预取器的结果。

## 9. 重复性与统计方法

- 功能与快速扫描可先执行 1 次；进入报告的配置至少使用 3 个独立运行或可证明确定性的重复实验。
- 若 gem5 配置完全确定，应通过统计量一致性验证；若包含随机替换、OS 时序或随机图，则固定并报告 seed。
- 报告均值、标准差和 95% 置信区间；图中保留单个样本点或误差条。
- ROI 前采用相同 warm-up，或使用 checkpoint 保证各配置从一致状态开始。
- 保存 `config.ini/config.json`、完整命令、git commit、benchmark binary hash 和原始 `stats.txt`。
- 正式比较前确认 instruction count 和算法输出一致，防止错误执行路径造成虚假加速。

## 10. 结果表模板

### 10.1 单配置摘要

| Workload/Input | Config | Runtime | Speedup vs none | Speedup vs best core prefetcher | Accuracy | Coverage | DRAM traffic Δ | CHI traffic Δ | Energy Δ | EDP Δ |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
|  |  |  |  |  |  |  |  |  |  |  |

### 10.2 机制归因

| Workload | Baseline misses | Eliminated misses | Useful-timely | Useful-late | Pollution misses | Remote shared | Owner transfer | DRAM misses | Helper active cycles |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
|  |  |  |  |  |  |  |  |  |  |

## 11. 预实验决策树

1. **内存瓶颈不明显：**扩大输入或更换 ROI；若仍不明显，该 workload 不作为核心论据。
2. **oracle 无收益：**检查落点、及时性和实际瓶颈；不要继续增加 helper 复杂度。
3. **oracle 有收益、helper 无收益：**优化地址生成、通信队列、预取距离或 helper 频率。
4. **覆盖率高但性能无收益：**检查 lateness、带宽拥塞、MSHR、cache pollution 和 CHI 队列。
5. **性能提升但能量恶化：**降低 helper 频率/活跃时间，增加 gating，或将结论限定为性能场景。
6. **只在单一图上有效：**分析图结构和算法 phase，明确适用范围，不以平均值掩盖负结果。
7. **明显优于 none 但不及 SPPv2：**判断是否具备目标核低开销、可编程性或复杂地址覆盖等其他价值。

## 12. 预实验完成标准

满足以下条件后，再进入大规模正式实验：

- ARM64 八核 Ruby CHI 启动和一致性 microbenchmark 全部通过；
- 至少两个图负载被确认存在显著可预取的访存瓶颈；
- oracle 实验表明 CHI 中存在有效的数据落点和预取窗口；
- 1× A55-like helper 能产生可追踪、可分类且具有正向性能收益的请求；
- 与 none、BOP、SPP/SPPv2 的比较口径统一；
- 性能收益能够由 miss 消除、及时性和 CHI/DRAM 流量解释；
- 至少一个代表性配置没有明显恶化全系统 EDP；
- 所有配置、命令、原始统计和正确性输出均可复现。

预实验最终应回答的不是“跨核预取平均加速多少”，而是：**它在哪类图访问模式、何种 CHI 数据路径和怎样的 helper/目标核频率组合下成立，以及收益何时会被通信、污染、带宽或能量代价抵消。**
