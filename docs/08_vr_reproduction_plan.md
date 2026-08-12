# Vector Runahead (ISCA 2020) 复现点

分支：`vr-repro` ｜ 建立日期：2026-08-12 ｜ 参考论文：`docs/paper/Vector_Runahead_Ajeya.pdf`

> 本文档是本分支的"复现点"（reproduction checkpoint）：它把论文《Vector Runahead》
> 拆成可验证的机制清单、可复现的指标、以及分阶段实现计划。每个 Stage 完成后，在
> 文末"进度台账"里登记证据并更新判定。推荐对外表述见 §7。

---

## 1. 论文信息

| 项 | 内容 |
|---|---|
| 标题 | Vector Runahead |
| 作者 | Ajeya Naithani, Sam Ainsworth, Timothy M. Jones, Lieven Eeckhout |
| 单位 | Ghent University, University of Edinburgh, University of Cambridge |
| 会议 | ISCA 2020 |
| 核心主张 | 在乱序核上，通过把 runahead 指令流**推测性向量化**（512-bit，含 gather），
  一条预取链式间接访问的**整条依赖链**，达到 1.79× 的几何平均加速比（相对 OoO 基线），
  且新增硬件状态仅 1.3 KB。 |

论文原文（扫描版 PDF）收录在本分支 `docs/paper/Vector_Runahead_Ajeya.pdf`。

### 1.1 一句话定位

> 传统 runahead 遇到链式间接访问（`A[i] → B[hash(A[i])] → data`）时，只能预取链头的
> striding load；Vector Runahead 在 runahead 模式下把标量指令**批量向量化**成 512-bit
> 向量/ gather，一次发出多条迭代的多个依赖层 load，从而为整条依赖链生成 MLP。

### 1.2 与仓库现有 PRE / DVR 的关系（重要）

本仓库 `main` 分支已有的原型是 **ISCA 2023《Decoupled Vector Runahead》(DVR)**——作者
相同、机制同源（stride 检测、taint 跟踪、VRAT/RDQ、控制流验收），但 DVR 是"独立子线程、
提前于主线程主动预取、有序轻量子线程"；本论文 VR 是"核内 runahead 模式下**不改 OoO 结构**、
以向量化压榨 MLP"。

更关键的是：**本仓库 gem5 fork 内已有一个可用的 PRE 实现**——它正是上游
[`lshpku/gem5-runahead` @ `dev-pre`](https://github.com/lshpku/gem5-runahead)（HPCA'20
PRE 的官方 gem5 实现）的直接拷贝，位于 `code/gem5-runahead-dev-pre/`（由外层 `vr-repro`
分支追踪），含 `enablePRE`/`enablePREBranch`/`enablePREEarlyRecycle`/
`numPRDQEntries`(=192)/`numSSTEntries`(=128) 参数、full-window stall 进入/退出、RDQ
回收、SST 与 MispTable，上游配置 `examples/three_level_o3.py`。本分支的 VR 改动直接落在
该 gem5 源码树上。冒烟脚本 `scripts/run_remote_pre_smoke.sh`。

**实现基座决策：在 PRE 之上增量实现 VR，复用 DVR 的共享构建块。**
论文将 VR 定义为 PRE 的增量修改（改终止条件、复用 PRE 的 RDQ、去掉 PRE 的
stalling-slice table），因此 **OoO → PRE → VR** 是论文 Figure 7 的原生对照链，也是本分支
验证 VR 性能的方式。三部分来源见 §5。**本分支不复制、不改写 `main` 的 DVR 逻辑，
只在其上做只读复用与 VR 新增。**

---

## 2. 论文核心机制拆解（复现对象）

按论文 Section III 逐条列出机制、硬件结构和参数。这是复现必须覆盖的"机制全集"。

### 2.1 进入 runahead 模式（III-C）
- 触发条件（load 阻塞 ROB 头时二者满足其一）：(1) ROB 被填满；(2) Issue Queue 填到
  满容量的 80%。
- 进入时 checkpoint：PC + 前端 RAT（每 RAT 项一个 checkpoint，叠在分支误预测恢复
  checkpoint 之上）。恢复时用 checkpoint 还原，前端重定向到 ROB 最后一条已派发指令之后。
- 进入 runahead 后、未遇到 striding load 之前：行为等同传统 runahead（仅寄存器回收 +
  高效 checkpoint），**不需要** PRE 的 fully-associative stalling-slice table。

### 2.2 步幅检测 Stride Detector（III-B）
- 结构：Reference Prediction Table（RPT，类似 [19]），按 load PC 索引，每项 4 字段：
  1. 上次访问地址（48-bit）
  2. 上次观测步幅（16-bit）
  3. 2-bit 饱和置信度计数器
  4. **terminator**：该 striding load 依赖链里最后一个 dependent load 的 PC（新字段，
     在 runahead 过程中填写，用于提前终止，§2.9）
- 触发：进入 runahead 后，解码到 **confidence = 3** 的 striding load 即进入 vector-runahead
  模式，对它及其依赖链向量化；直到再次遇到同 PC 的 striding load 动态实例或依赖链完成
  （§2.9）。两个 striding load 动态实例之间的指令称 **indirect chain（间接链）**。
- 论文参数：32 项，48-bit 地址 + 16-bit 步幅 + 2-bit 计数 + 48-bit terminator ≈ 456 B。

### 2.3 污点向量 Taint Vector (TV)（III-D）
- 结构：每个架构整数寄存器一个入口，2 个标志位：`vectorize`（上一条写该寄存器的指令被
  向量化）、`invalid`（上一条写该寄存器的指令无效）。runahead 开始清空，终止时也清空。
- 初始化：被发现的 striding load 的目的寄存器置 `vectorize`；不支持的运算（如浮点输入）
  的目的寄存器置 `invalid`。
- 传播：向量污点跟踪——任一输入被标记则目的也标记；无输入被标记则目的清标记。
- 分类执行：无标记 → 常规标量 runahead 指令（对当前 VR 迭代视为 loop-invariant）；
  `invalid` → 丢弃；仅 `vectorize` → 向量化。
- 论文参数：16 个寄存器 × 2 bit = 4 B。

### 2.4 指令向量化 Vectorizing Instructions（III-E）
- 微程序例程生成输入标量指令的向量版本；对 striding load，用当前地址 + 步幅生成
  512-bit 向量 load（gather）注入流水线。
- 一律使用 512-bit 向量寄存器（对齐 Intel AVX-512）；8 个标量操作数塞进一个 512-bit 向量
  （任意 ≤64-bit 操作数）；复用微架构的物理向量寄存器和向量单元微操作。
- 算术和 load 指令（直接或间接依赖 striding load）都向量化；依赖链 load 全部变
  gather——多级间接（pointer-chasing）时，把链上所有 load 向量化成 gather，并把 stride
  table 的 terminator 更新为链末 gather load 的 PC。
- lane 级掩码：某 lane 产生非法地址 → 该 lane 标 invalid，后续向量指令对应 lane 被 mask。
- **runahead 模式不分配 ROB 项**（状态不保留），改用 RDQ 管理寄存器回收（§2.8）。
- 忽略：浮点指令（标记为 invalid，见 [57]）、store、原代码已向量化的指令。

### 2.5 控制流 Control Flow（III-F）
- 假设：所有 lane 走相同控制流。遇到分支时用微操作把标量分支转成 8-lane predicate mask。
- 用**第一个 lane**的结果决定分支方向，其他走不同路径的 lane 被 mask 掉，mask 持续到当前
  VR 迭代终止。
- 不同 unrolled 迭代（§2.6）之间可独立控制流。

### 2.6 向量展开与流水化 Vector Unrolling & Pipelining（III-G）
- **Vector Unrolling (U)**：第 1 轮完成后，对 striding 序列的下 N 个值再发一个向量 load，
  重复直到发出 U 份向量化序列副本。N=8 且 U=8 时 = 64 个原始循环迭代。
- **Vector Pipelining (P)**：软件流水化式重排——不等上一轮完成就同时发出多份每条向量指令
  （不同 lookahead 距离的 stride 输入），标量与向量指令形成一对多映射，重叠多个 unrolled
  迭代的 load 执行，突破单 gather 8 load 的 MLP 上限。
- 默认 U = P = 8（论文实现硬编码），可发 64 个标量等价的并行 gather load。
- 寄存器复用：不同 pipeline group 的向量可复用物理寄存器（不同时活）；pipelining 会拉长
  活跃区间并增大 VRAT（§2.7）。

### 2.7 向量寄存器分配表 VRAT（III-H）
- 需要把架构标量寄存器重命名为**物理向量寄存器**；pipelining 下一对多，P 深度需要把 1 个
  架构标量寄存器重命名为 P 个物理向量寄存器。
- 结构：每个架构整数寄存器 P 个入口，记录该寄存器被赋给 P 个 pipelined 副本的物理向量
  寄存器号。新向量指令从对应入口取源，区分各 pipeline 迭代的输入输出。
- 论文参数：16 个整数寄存器 × P(=8) 入口 ≈ 112 B。

### 2.8 寄存器回收队列 RDQ（III-I）
- 物理寄存器不能乱序释放（OoO 核只在提交新写同一架构寄存器的指令时才释放旧映射；runahead
  不提交）。VR 用**按序 RDQ**（PRE [64] 同款）在寄存器不再用于地址生成时即释放。
- 每条指令查 VRAT，找到"最后一次写同架构寄存器"的 P 个物理向量寄存器，等新指令到达流水线
  末端后这些旧寄存器即死。每条向量化指令一条 RDQ 项；无效指令（store 等）不占项。
- 维护 head 指针指向第一条未执行指令；head 处指令已执行即释放其寄存器并前移。
- 论文参数：192 项 × 4 B = 768 B。

### 2.9 终止 Vector Runahead（III-J）
满足任一即终止：
1. 再遇到同一 striding load 的动态实例；
2. 遇到并发出 terminator（stride table 里依赖链末 load 的 PC）；
3. 所有 lane 被标 invalid；
4. 超时（vector-runahead 模式执行了 200 个标量等价指令）。

U > P 时，终止后**立即**用下一个 striding load 重新进入 VR，重复直到发满 U/P 轮才恢复
正常执行。终止后：恢复前端 RAT 到进入点，清空 TV/VRAT/RDQ，前端重定向到 ROB 最后一条
已派发指令之后。

### 2.10 硬件开销（III-K）

| 结构 | 参数 | 大小 |
|---|---|---|
| Stride Detector | 32 项 (48b addr + 16b stride + 2b cnt + 48b term) | 456 B |
| Taint Vector | 16 regs × 2 bit | 4 B |
| VRAT | 16 regs × 8 entries | 112 B |
| RDQ | 192 项 | 768 B |
| **合计** | | **1.31 KB** |

对比：PRE 为 1.24 KB。

---

## 3. 复现环境

### 3.1 论文实验环境（Sniper 6.0，目标"行为级"对齐对象）

- 模拟器：Sniper 6.0，基于 Intel Skylake 的乱序核模型（Table I）：
  - 3.2 GHz OoO，ROB 224，Issue Queue 97，Load Queue 64，Store Queue 60
  - 4-wide fetch/dispatch/rename，8-wide commit，8 级前端
  - TAGE-SC-L 8 KB 分支预测器
  - 功能单元：3 int add (1c), 1 int mult (3c), 1 int div (18c), 1 fp add (3c),
    1 fp mult (5c), 1 fp div (6c)
  - 寄存器：180 int (64b) + 180 fp (128b) + 96 vector (512b)
  - L1I 32 KB assoc4 2c；L1D 32 KB assoc8 4c + **stride prefetcher (16 streams)**；
    L2 256 KB assoc8 8c；L3 8 MB assoc16 30c
  - 内存：45 ns 最小延迟，51.2 GB/s 带宽，request-based contention model
  - 24 MSHRs
- 工作量：跳过初始化，每个 workload 的 ROI 跑 2 亿条指令。
- 编译：`-O3 -ftree-vectorize`（论文确认 autovectorization 不改变性能）。

### 3.2 本分支适配平台（gem5/RISC-V）

与 `main` 分支 DVR 一致：**ISA 适配到 RISC-V/gem5**，不是逐周期复刻 Sniper/x86/AVX-512
绝对性能数字。本分支目标是在 gem5 的 RISC-V OoO 核上实现 VR 机制、证明机制行为与论文一致，
而不是精确复现 1.79× 这一绝对数。

**对照配置三件套**（对齐论文 Figure 7）：同一 gem5/RISC-V DerivO3CPU 环境下的
`OoO`（无 PRE 无 VR）、`PRE`（`enablePRE=True`，fork 已有）、`VR`（本分支新增）。
三个配置共享同一套 workload 与指标采集，确保对照干净。

关键适配点：
- AVX-512 512-bit / 8-lane → gem5 RISC-V **向量扩展（RVV）**或自定义向量物理寄存器
  （需确认 gem5 该 fork 的 RVV 支持程度，见 §6 风险 1）。
- Sniper 的 stride prefetcher → gem5 现有 stride prefetcher 或复用 DVR 的 RPT 逻辑。
- ROB 224 / IQ 80% 进入条件 → 映射到 gem5 OoO 配置参数。

---

## 4. 复现指标与验收标准

论文 §VI 的量化主张，作为本分支"机制生效"的验收靶。所有指标统一按
**OoO vs PRE vs VR** 三对照口径采集（PRE 用 fork 内 `enablePRE` 实现），
对齐论文 Figure 7/8/12/13 的结构：

| 指标 | 论文数值 | 本分支验收口径 |
|---|---|---|
| 整体性能 | VR 几何平均 **1.79×** vs OoO 基线；**1.49×** vs PRE；PRE 相对 OoO 1.20× | 三对照报告 speedup；VR > PRE > OoO 的单调关系是核心断言（非必须复刻绝对值） |
| MLP | VR 相对 OoO 生成 **2.3×** MLP（图 8，按 MSHR 占用口径） | 同口径报告 MLP 提升 |
| Coverage | 正常模式内存访问占比：VR **11.5%** vs PRE 43%（图 12） | 报告正常/runahead 模式 off-chip 访问占比 |
| Accuracy | 预取 cacheline 中被正常模式访问的比例 > 90%（图 13a） | 同口径报告 |
| Timeliness | 预取命中位置分布 L1D/L2/L3/Off-chip（图 13b） | 同口径报告 |
| 敏感性 | U/P、LLC size、MSHR 数（图 9–11） | 至少复现 U/P 和 MSHR 两组的单调趋势 |
| 开销 | 新增硬件 1.31 KB（表 III-K） | 列出所有新增结构尺寸并核对 < 2 KB 量级 |

每项至少覆盖：**Camel、Hash Join (HJ2)、Kangaroo、RandAcc**（间接链 ≥ 2、有算术的关键
代表），有精力再补 Graph500、NAS-CG/IS。

---

## 5. 实现基座：PRE（fork 已有）+ 复用 DVR 共享逻辑

**核心决策：在 PRE 上增量实现 VR。** 理由：

1. 论文把 VR 定义为 PRE 的增量修改——VR *"alters the runahead's termination
   condition"*、复用 *"the RDQ, with 192 entries as used by PRE"*、并 *"eliminates the
   need for [PRE's] fully-associative stalling-slice table"*。因此 OoO → PRE → VR 是
   论文 Figure 7 的原生对照链，也是本分支验证 VR 性能的方式。
2. **本仓库 gem5 fork 已有可用的 PRE 实现**：`src/cpu/o3/pre.{cc,hh}`（22 KB / 372 行），
   已接入 DerivO3CPU 的 commit/rename/iew，含 `enablePRE`/`enablePREBranch` 参数、
   full-window stall 进入/退出、RDQ 寄存器回收、SST（stalling-slice table）与
   MispTable，并有 `scripts/run_remote_pre_smoke.sh` 冒烟脚本。在 PRE 上改，比从 DVR
   或从头起步都更贴近论文、风险更低。
3. **PRE 的开源现状**：论文正文与 [CAL/HPCA 版本](https://personales.upv.es/jofepre/docs/CAL_2019.pdf)
   均为开放获取，但公开渠道未见官方代码仓库；不影响本方案——fork 内已有实现。

### 5.1 三部分来源

| 部分 | 来源 | 在本分支的作用 |
|---|---|---|
| **runahead 核心** | PRE（fork 已有） | 进入条件（ROB 满 / IQ 80%）、PC/RAT checkpoint 与恢复、dispatch 过滤、RDQ 寄存器回收、stalling load 返回退出 |
| **共享构建块** | DVR（Stage 1–13 只读复用） | RPT stride 检测（补 terminator 字段）、taint 跟踪（VTT → TV 的 vectorize/invalid bit）、VRAT/VIR 重命名簿记、控制流 predicate/reconvergence |
| **VR 独有** | 本分支新增 | 向量化器（标量→512-bit 向量/gather）、置信度=3 进入向量化、四项终止条件、U=P=8 展开+流水化、非法 lane 掩码 |

### 5.2 与 DVR 实现的重叠对照

| 机制 | 本论文（VR） | 现有 DVR 实现 | 复用方式 |
|---|---|---|---|
| stride 检测 | RPT + confidence + terminator | 32-entry RPT / stride candidate（Stage 1） | 只读复用，补 terminator 字段 |
| taint 跟踪 | TV：vectorize/invalid 2 bit | 32-register VTT/FLR（Stage 4） | 只读复用 |
| 间接链/多级 dependent | gather 链 + terminator | 两级 dependent prefetch（Stage 7–8） | 复用思想，实现改为 gather 形态 |
| 寄存器簿记 | VRAT + RDQ | VRAT/VIR + 8-uop recorder（Stage 10） | 复用 VRAT，RDQ 直接来自 PRE |
| 控制流 | 1st-lane 方向 + mask | predicate/reconvergence（Stage 11–12） | 复用 |
| 分支/循环推断 | （无，靠 RPT + terminator） | backward branch / loop bound（Stage 5–6） | 可选增强 |

**VR 特有的、需要新写的部分：**
1. **向量化器**：标量指令 → 512-bit/RVV 向量指令 + gather 生成（PRE 是标量 runahead；
   VR 是真正的向量指令形态）。
2. **向量化进入**：runahead 内解码到 **confidence=3** 的 striding load 才进入向量化
   （PRE 无此阶段）。
3. **终止条件四项**：再遇同 striding load / terminator / 全 lane invalid / 200 指令超时
   （PRE 是 stalling load 返回即退）。
4. **Unrolling (U) + Pipelining (P)**：多轮向量化 + 软件流水化重叠（全新结构，U=P=8）。
5. **lane 掩码语义**：非法 lane → 后续向量指令 mask（DVR 是 actual-value predicate）。

### 5.3 VR 挂载点（代码级，基于 2026-08-12 对 PRE/DVR 源码的梳理）

| # | 位置 | 现状 | VR 改动 |
|---|---|---|---|
| 1 | `commit.cc:748-771` | PRE 进入：ROB 满 + 头部 load 未就绪 → `cpu->enterPRE()` | 不动；VR 是 PRE 内的子模式 |
| 2 | `cpu.cc:2690/2722` `enterPRE/exitPRE` | checkpoint/恢复 RAT + free list，`inPRE` 标志 | 增加 `inVR` 标志与 VR checkpoint |
| 3 | `commit.cc:805-830` | PRE 退出：stalling load `readyToCommit()` → `exitPRE()` + `squashDueToPRE` | 在 VR 子模式下替换为四项终止条件（§2.9） |
| 4 | `pre.hh:32` `DVRStrideDetector` | 32 项 RPT：地址/步幅/置信度/年龄，无 terminator；阈值=2 | 新增 VR stride 表：阈值 **confidence=3** + **terminator** 字段（链末 dependent load PC） |
| 5 | `pre.hh:110` `DVRVectorTaintTracker` | 32 int reg 单 bit taint，src→dst 传播，记 FLR | 新增 VR TV：**2-bit/reg**（vectorize + invalid），propagate 逻辑同构 |
| 6 | **向量化器**（全新） | 无 | 标量指令 → 512-bit/RVV 向量 + gather；微程序例程生成向量指令注入流水线 |
| 7 | `pre.hh:199` `DVRVectorRenameTable` | 32 arch × 8 chunk × 128 phys（DVR 用） | VR 的 **VRAT**：P(=8) 项/arch reg，指向物理向量寄存器 |
| 8 | `BaseO3CPU.py:185` `numPRDQEntries` / `enablePREEarlyRecycle` | PRE 的 PRDQ 已存在 | VR 的 **RDQ** 直接用 PRE 的 PRDQ 语义（按序回收） |
| 9 | `rename.cc` / `commit.cc:1515` | PRE 模式 dispatch 过滤已存在 | 在 rename/dispatch 处检测 confidence=3 striding load 并切向量化路径 |
| 10 | **Unroll+Pipeline 状态机**（全新） | 无 | U=P=8 轮次控制、向量流水化重叠、MSHR 饱和 |

**向量化器的关键依赖**：需要 gem5 RISC-V 的向量指令（RVV）支持或自建向量物理寄存器，
这决定 Stage 0 必须先确认（§6 Stage 0）。

---

## 6. 分阶段实现计划

沿用仓库 `Stage N` 惯例，每个 Stage 有独立验收脚本和硬断言。**基座 = PRE**（fork 已有），
实现顺序如下（每阶段可单独提交）：

| Stage | 内容 | 验收点（证据类型） | 依赖 |
|---|---|---|---|
| **0** | PRE 基座验证 + 三对照配置就绪：`enablePRE` 冒烟通过、OoO/PRE 两组 config 可跑、RVV/向量指令支持确认 | PRE 冒烟脚本通过；三配置 config.ini 逐项检查 | — |
| **1** | PRE → VR 桥接：runahead 内识别 striding load（置信度=3）+ stride table 补 terminator 字段 | 向量化触发计数、terminator 更新正确 | Stage 0 |
| **2** | 向量化器 v1：striding load → 向量 load/gather，算术依赖指令 → 向量指令 | 向量指令生成数、lane 数正确 | Stage 1 |
| **3** | TV 污点传播 + invalid 丢弃（复用 VTT 结构，改造为 vectorize/invalid 2-bit） | tainted/invalid 事件计数，对照 DVR Stage 4 | Stage 2 |
| **4** | VRAT + RDQ：向量重命名（扩展 P 深度）+ 按序寄存器回收 | 重命名守恒、RDQ 释放正确、无寄存器泄漏断言 | Stage 3 |
| **5** | 四项终止条件 + U/P 轮次控制（U>P 时立即重入） | 各类终止计数、轮次守恒 | Stage 4 |
| **6** | 控制流 mask（1st-lane 方向 + 非法 lane mask，跨 lane 持久） | divergent/masked 计数、掩码持久性 | Stage 5 |
| **7** | Unrolling (U=8) + Pipelining (P=8) 完整实现 | 64 标量等价 gather 发出、MLP 提升 | Stage 6 |
| **8** | 微基准：Camel/HJ2/Kangaroo/RandAcc 移植 + 编译 | 可跑通、ROI 提取 | Stage 7 |
| **9** | 三对照指标采集：**OoO vs PRE vs VR**（speedup/MLP/coverage/accuracy/timeliness） | 与 §4 表格口径对齐；PRE 作为对照组 | Stage 8 |
| **10** | 敏感性：U/P、MSHR、LLC 扫描 + 完整回归脚本（含 PRE 对照） | 趋势一致 + 全回归硬断言通过 | Stage 9 |

每个 Stage 完成 = 写清证据（事件计数、config 检查、与 PRE/DVR/论文对照）、脚本硬断言
通过、在本文档"进度台账"登记。**不以绝对 speedup 数字作为完成门槛**（本平台无法逐周期
对齐 Sniper），以"机制行为与论文一致"为准。Stage 9 的输出 = OoO/PRE/VR 三列对照表，
直接对应论文 Figure 7 的结构。

---

## 7. 推荐对外表述

> We implement an ISA-adapted prototype of Vector Runahead on a RISC-V
> microarchitecture, reusing the DVR substrate for stride detection, taint
> tracking and register bookkeeping, and adding speculative vectorization
> (gather), VR entry/termination conditions, and unroll+pipeline (U=P=8)
> MLP boosting. We validate mechanism behavior against the ISCA 2020 paper's
> qualitative claims on representative indirect-chain microbenchmarks.

---

## 8. 风险清单

1. **gem5 RISC-V 向量支持**：RVV 在 gem5 的成熟度（完整 512-bit 向量/ gather 是否可用）
   ——若不足，退化方案为"自定义物理向量寄存器 + 模拟 gather"。
2. **绝对性能不可比**：Sniper 6.0/Skylake 配置 vs gem5/RISC-V 配置差异大，1.79× 为论文
   环境数字，本分支只做行为级验证。
3. **微基准质量**：论文用真实 workload ROI；本分支用移植微基准（Camel/HJ2/Kangaroo/
   RandAcc），需保证间接链结构与论文一致。
4. **RDQ/VRAT 状态守恒**：多轮 pipelining 下物理向量寄存器泄漏/悬空是最易出的正确性 bug。
5. **与 DVR 分支的耦合**：复用时确保不把 DVR 特有语义（独立子线程、提前主动预取）混入
   VR 的 runahead 语义。

---

## 9. 进度台账（checkpoint）

| Stage | 状态 | 证据摘要 | 判定 |
|---|---|---|---|
| 0 | 结论完成，脚本就绪（待服务器运行） | RVV 确认：fork 无真实 RVV（`DummyVecRegContainer`），向量化走 DVR 同款"模拟向量物理寄存器 + L1D timing port 真实预取"；`scripts/run_remote_vr_stage0_prebase.sh` 提供 OoO/PRE 两组冒烟与 `numCycles`/L1D 缺失留底 | 待 `VR_STAGE0_PREBASE_PASSED` |
| 1 | 代码完成，脚本就绪（待服务器编译运行） | `vr.{hh,cc}`（confidence=3 RPT + terminator 字段、2-bit TV、round/prefetch 结构）；cpu.hh/cc：`observeVRLoad/enterVR/observeVRInstruction/issueVRGather/serviceVRPrefetchQueue/completeVRPrefetch/replayVRChain/exitVR` + 16 项 vr\* 统计；钩子：commit.cc 两处退出、lsq_unit.cc 观察点、rename.cc 观察点、lsq.cc 响应分发；`BaseO3CPU.py` 6 个 VR 参数；`scripts/run_remote_vr_stage1_smoke.sh` 断言 rounds/gathers/issued/completed/tainted 全正 | 待 `VR_STAGE1_SMOKE_PASSED` |
| 2 | 待开始 | — | — |
| 3 | 待开始 | — | — |
| 4 | 待开始 | — | — |
| 5 | 待开始 | — | — |
| 6 | 待开始 | — | — |
| 7 | 待开始 | — | — |
| 8 | 待开始 | — | — |
| 9 | 待开始 | — | — |
| 10 | 待开始 | — | — |

> 本分支建立时 `main` HEAD = `920a20e`（DVR Stage 13 全回归通过）。
