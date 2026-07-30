# Slipstream Processors Revisited: Exploiting Branch Sets

> 中文题目：重访 Slipstream 处理器：利用分支集合  
> Vinesh Srinivasan, Rangeen Basu Roy Chowdhury, Eric Rotenberg  
> ISCA 2020, DOI: 10.1109/ISCA45697.2020.00020  
> 原文：[03.Slipstream Processors Revisited- Exploiting Branch Sets.pdf](03.Slipstream%20Processors%20Revisited-%20Exploiting%20Branch%20Sets.pdf)

## 如何使用这份笔记

这是一份面向精读的中英对照版，而不是简单 OCR：每个英文段落后紧跟中文翻译；双栏 PDF 中被拆开的句子、断词和图表标题已经按语义复原。为了让篇幅可读，相关工作的连续短段落和硬件流程列表有少量合并，但没有省略论文的核心论证。参考文献只保留引用编号，不翻译作者、刊名与页码。

---

# 0. 阅读前必须掌握的基础知识

## 0.1 这篇论文究竟要解决什么问题？

处理器经常被两类“麻烦指令”拖慢：

- **Delinquent branch（顽固/问题分支）**：频繁预测错误的分支。
- **Delinquent load（顽固/问题加载）**：频繁发生 Cache Miss 的加载指令。

最糟糕的情况是二者相遇：一次长延迟 load 的结果被后续 branch 使用，而这个 branch 又预测错误。乱序处理器虽然能在 load 等待期间向前执行，但分支错误一旦确认，错误路径上的工作全部被清空，之前隐藏内存延迟的努力也就浪费了。

## 0.2 Leader-Follower（领导者-跟随者）

Slipstream 同时运行同一程序的两个副本：

- **A-stream（Advanced stream，先行流）**：删掉一部分指令，跑在前面，负责提前产生分支结果和预取数据；它是推测性的。
- **R-stream（Redundant stream，冗余流）**：运行完整程序，负责保证正确性；它使用 A-stream 提前得到的信息。

可以把它想象成“侦察兵 + 正式部队”：侦察兵轻装前进，正式部队根据侦察信息选择正确路线。

## 0.3 控制依赖、数据依赖与汇合点

```c
if (x > 0) {          // 分支 B
    r4 = foo();       // 依赖 B 的结果才决定是否执行
} else {
    r4 = bar();
}
use(r4);              // 两条路径在这里附近重新汇合
```

- `foo()` 和 `bar()` 所在区域对 B **控制依赖**（Control-Dependent, CD）。
- `use(r4)` 对前面的赋值具有**数据依赖**。
- 两条路径再次相遇的位置叫 **reconvergent point（控制流汇合点）**。

## 0.4 Backward slice 与 Forward slice

- **Backward slice（后向切片）**：为了计算某条目标指令，需要向前追溯哪些生产者指令。
- **Forward slice（前向切片）**：某条指令产生的值或控制决策，会继续影响后面的哪些指令。
- 本文特别使用 **forward control-flow slice（前向控制流切片）**：目标分支的 CD 区域、受其数据影响的后续分支，以及这些后续分支各自的 CD 区域。

上一篇 Speculative Precomputation 主要抽取 delinquent load 的 backward slice；本文最重要的转变是从 A-stream 中删除 delinquent branch/load 的 **forward control-flow slice**。

## 0.5 CIDI 与 CIDD

- **CIDI branch**：Control-Independent, Data-Independent，控制独立且数据独立的分支。相对于目标分支/load，它没有受到目标的控制结果或数据结果影响。
- **CIDD branch**：Control-Independent, Data-Dependent，控制独立但数据依赖的分支。它虽然不位于目标分支的 CD 区域中，却读取了由该区域直接或间接产生的值。
- **Branch set（分支集合）**：相对于某个目标分支或 load，所有 CIDD 分支组成的集合。

如果一个 delinquent branch 出现在自己的 branch set 中，说明下一次动态实例依赖上一次实例的结果，它是串行递归依赖，不能安全地提前执行。

## 0.6 Pre-execution 与 Prefetch 的区别

- **Pre-execution（预执行）**：提前真正执行一部分程序，既可以得到分支方向，也可以触发内存访问。
- **Prefetch（预取）**：提前请求数据，结果不能改变架构状态。
- 本文把 A-stream 中的 delinquent load 转成 **non-binding prefetch（非绑定预取）**：它只把数据带入 Cache，不向后续指令提供必须正确的架构结果。

## 0.7 MPKI、IPC、SimPoint、EDP

- **IPC**：每周期完成的指令数，越高通常越好。
- **MPKI**：每千条指令的 Miss/Misprediction 数。
- **SimPoint**：从长程序中挑选具有代表性的执行区间，以降低模拟成本。
- **EDP**：Energy-Delay Product，能量与执行时间的乘积；同时衡量节能和性能。

---

# 1. Abstract / 摘要

**English**

Delinquent branches and loads remain key performance limiters in some applications. One approach to mitigate them is pre-execution. Broadly, there are two classes of pre-execution: one repeatedly forks small helper threads, each targeting an individual dynamic instance of a delinquent branch or load; the other begins with two redundant threads in a leader-follower arrangement, and speculatively reduces the leading thread. The objective of this paper is to design a new pre-execution microarchitecture that meets four criteria: (i) retains the simpler coordination of a leader-follower microarchitecture, (ii) is fully automated with just hardware, (iii) targets both branches and loads, and (iv) is effective. We review prior pre-execution proposals and show that none meet all four criteria.

**中文翻译**

顽固分支和顽固加载仍然是一些应用中的关键性能瓶颈。一种缓解方法是预执行。广义上，预执行分为两类：第一类不断派生小型辅助线程，每个辅助线程针对某个顽固分支或加载的一个动态实例；第二类从两个采用领导者-跟随者关系的冗余线程开始，并以推测方式精简先行线程。本文希望设计一种同时满足四项标准的新型预执行微体系结构：（1）保留领导者-跟随者结构较简单的协调方式；（2）仅靠硬件即可完全自动化；（3）同时针对分支和加载；（4）确实有效。作者回顾了以前的预执行方案，指出它们没有一个同时满足全部四项标准。

**English**

We develop Slipstream 2.0 to meet all four criteria. The key innovation in the space of leader-follower architectures is to remove the forward control-flow slices of delinquent branches and loads from the leading thread. This overcomes key limitations in the only other hardware-only leader-follower works: Slipstream and Dual Core Execution (DCE). Removing forward control-flow slices enables leader-follower branch pre-execution without relying on confident-instruction removal, and tolerance of cache-missed loads that feed mispredicted branches.

**中文翻译**

作者提出 Slipstream 2.0 来满足全部四项标准。它在领导者-跟随者体系结构中的关键创新，是从先行线程中删除顽固分支和顽固加载的前向控制流切片。这解决了另外两种纯硬件领导者-跟随者方案——原始 Slipstream 与双核执行 DCE——的关键局限。删除前向控制流切片带来两项能力：无需依赖“高置信度指令删除”也能进行领导者-跟随者式分支预执行；即使 Cache Miss 的 load 为一个预测错误的分支提供输入，也能够容忍这次 load 的延迟。

**English**

For SPEC 2006/2017 SimPoints wherein Slipstream 2.0 is auto-enabled, it achieves geometric-mean speedups of 67%, 60%, and 12% over baseline (one core), Slipstream, and DCE, respectively.

**中文翻译**

在自动启用 Slipstream 2.0 的 SPEC CPU 2006/2017 SimPoint 上，相对于基线单核、原始 Slipstream 和 DCE，其几何平均加速比分别达到 67%、60% 和 12%。

---

# 2. Introduction / 引言

**English**

Delinquent branches (frequently mispredicted) and loads (frequently cache-missed) remain major limiters of single-thread performance. They are even worse when they coincide: a cache-missed load feeding a mispredicted branch neutralizes the latency-hiding ability of large-window processors, as all instructions fetched in the shadow of the miss are squashed.

**中文翻译**

频繁预测错误的顽固分支与频繁 Cache Miss 的顽固加载，仍然严重限制单线程性能。当二者同时出现时尤其糟糕：如果一个 Cache Miss 的 load 为预测错误的 branch 提供输入，大指令窗口处理器隐藏延迟的能力就会失效，因为在等待 Miss 期间取入并执行的所有错误路径指令最终都会被清空。

**English**

Figure 1 compares a baseline core with the same core having perfect branch prediction and a perfect L1 data cache. The baseline uses a 5.5KB VLDP prefetcher and a 64KB TAGE-SC-L branch predictor. On selected SPEC 2006/2017 SimPoints, the perfect configuration shows more than 2x upper-bound speedup potential.

**中文翻译**

图 1 将基线核心与“完美分支预测 + 完美 L1 数据 Cache”的同一核心进行比较。基线已经配备 5.5KB 的 VLDP 预取器和 64KB 的 TAGE-SC-L 分支预测器；即便如此，所选 SPEC 2006/2017 区间在理想配置下仍表现出超过 2 倍的理论加速空间。这说明研究对象并不是一个已经被普通预测器和预取器完全解决的小问题。

**English**

Helper threads can resolve delinquent branches and initiate delinquent loads before the main thread fetches corresponding instances. One class repeatedly forks transient helper threads containing backward slices. The other starts two redundant leader-follower threads and prunes the leader while preserving accurate global control-flow.

**中文翻译**

辅助线程可以在主线程取到相应动态指令之前，就提前解析顽固分支并发起顽固加载。一类方法反复派生临时辅助线程，每个线程执行目标分支/load 的后向切片；另一类方法同时运行两个冗余的领导者-跟随者线程，通过删减领导者，同时保持全局控制流的正确对应关系。

## 2.1 四个设计目标与相关工作

**English**

The proposed microarchitecture should: (1) retain simple leader-follower coordination, (2) be fully automated in hardware, (3) target both branches and loads, and (4) be effective.

**中文翻译**

作者给新体系结构设定四个目标：（1）保持领导者-跟随者协调简单；（2）完全由硬件自动完成；（3）同时解决分支与加载；（4）在性能上真正有效。

**English**

Leader-follower coordination is attractive because the leader is continuously active and there is a one-to-one global control-flow correspondence between leader and follower. It avoids carefully timing the fork of each per-instance helper thread and aligning each pre-executed branch outcome with the right main-thread branch instance.

**中文翻译**

领导者-跟随者方式的吸引力在于：领导者持续运行，两个线程的全局控制流保持一一对应。这样不需要为每个动态实例精确选择辅助线程的派生时机，也不用艰难地把每个提前得到的分支结果与主线程中的正确动态分支实例对齐。

**English**

Slice processors, speculative precomputation, and continuous runahead are hardware-automated and effective for their targets, but they are not leader-follower schemes and primarily target loads. DDMT, speculative slices, and SSMT can target branches and loads, but are not leader-follower and require manual or compiler support rather than being purely hardware automated.

**中文翻译**

Slice Processor、Speculative Precomputation 和 Continuous Runahead 能由硬件自动运行，也能有效处理各自目标，但它们不是领导者-跟随者结构，而且主要针对 load。DDMT、Speculative Slices 和 SSMT 能同时针对分支与 load，却不是领导者-跟随者结构，并且需要人工识别、插入触发指令或编译器支持，而非纯硬件自动化。

**English**

Original Slipstream runs an A-stream and R-stream on two cores or SMT contexts. It removes confidently predicted branches and their backward slices from the A-stream, replacing them with confident predictions. The R-stream receives both those predictions and outcomes of unremoved branches pre-executed by the A-stream.

**中文翻译**

原始 Slipstream 在两个核心或两个 SMT 上下文中运行 A-stream 和 R-stream。它从 A-stream 中删除能够高置信度预测的分支及其后向切片，并用预测结果取代它们。R-stream 同时接收被删除分支的原预测结果，以及 A-stream 对未删除分支提前执行得到的真实结果。

**English**

Original Slipstream becomes ineffective in phases dominated by unpredictable branches: those are exactly the phases needing branch pre-execution, but there are too few confident branches and backward slices to remove. It also does not explicitly target loads; loads removed transitively with backward slices are lost instead of being converted into non-binding prefetches.

**中文翻译**

当执行阶段主要由难预测分支构成时，原始 Slipstream 效果很差：这本来是最需要分支预执行的时候，但高置信度分支太少，A-stream 无法删掉足够多的指令而领先。它也没有显式针对 load；随分支后向切片被连带删除的 load 直接消失，而没有被保留并转化为非绑定预取，因而丢掉了制造内存级并行性的机会。

**English**

DCE pseudo-retires a cache-missed load and its forward slice in the leading thread, discarding invalid results and dynamically turning the load into a non-binding prefetch. It works well unless a load-dependent branch is mispredicted. Then the leading thread must restart from the trailing thread, and the load latency reappears as branch recovery latency.

**中文翻译**

DCE 会在先行线程中“伪退休”发生 Cache Miss 的 load 及其前向切片：解除阻塞但丢弃无效结果，相当于把长延迟 load 动态转换为非绑定预取。只要依赖该 load 的分支预测正确，它就很有效；一旦该分支预测错误，先行线程必须从跟随线程的状态重新启动，原本想隐藏的 load 延迟便以分支恢复延迟的形式重新暴露。

**English**

DLA combines ideas from Slipstream and DCE, using offline profiling to remove confident branches and reintroduce delinquent loads as non-binding prefetches. It targets both branches and loads, but is not hardware-only. It inherits both branch-pruning limitations of Slipstream and the load-to-mispredicted-branch limitation of DCE.

**中文翻译**

DLA 结合 Slipstream 与 DCE：借助离线 profiling 删除高置信度分支，并把被删掉的顽固 load 重新放回先行线程作为非绑定预取。它确实同时处理分支和 load，却不是纯硬件方案；而且同时继承了 Slipstream 在难预测分支密集阶段删不动指令的局限，以及 DCE 无法有效处理“Miss load → 错误预测 branch”的局限。

## 2.2 Slipstream 2.0 的核心思想

**English**

Slipstream 2.0 begins with dual redundant A- and R-streams. Instead of removing backward slices of confident branches, it removes forward control-flow slices of hard-to-predict pre-executable branches and delinquent loads from the A-stream while preserving correct overall control-flow. A branch is pre-executable if it does not depend on itself: after removing its forward control-flow slice, the next dynamic instance can still execute correctly.

**中文翻译**

Slipstream 2.0 同样从冗余的 A-stream 与 R-stream 开始。但它不再删除高置信度分支的后向切片，而是从 A-stream 中删除“难预测但可预执行的分支”和“顽固 load”的前向控制流切片，同时维持整体控制流正确。如果一个分支不依赖自身，也就是删掉当前实例的前向控制流切片后，下一个动态实例仍能正确执行，那么它就是可预执行分支。

**English**

A branch set is the list of control-independent, data-dependent (CIDD) branches with respect to a pre-executable branch or load. It serves two purposes: a branch is pre-executable if it is not in its own branch set, and the branch set identifies the forward control-flow slice that must be removed.

**中文翻译**

Branch set 是相对于某个可预执行分支或 load 的所有“控制独立、数据依赖”（CIDD）分支列表。它有两个作用：第一，若目标分支不在自己的 branch set 中，它才可预执行；第二，branch set 告诉硬件，为完整删除目标的前向控制流影响，还必须跳过哪些后续分支及其控制依赖区域。

**English**

Hardware automatically identifies delinquent branches/loads, predicts branch reconvergent points to delineate control-dependent regions, and finds probable CIDD branches using forward poisoning through CD regions and data-dependent instructions.

**中文翻译**

硬件自动完成三件事：识别顽固分支和 load；预测分支的汇合点，以界定控制依赖区域；通过从 CD 区域向前传播“毒化标记”，识别潜在的 CIDD 分支。

**English**

For Delinquent Branch Pre-execution (DBP), the A-stream fetches and retains the target branch, skips its CD region, then discards each branch in its branch set and skips each corresponding CD region. The target branch therefore executes early without requiring prediction in the A-stream. Its predicate becomes a highly accurate prediction for the R-stream.

**中文翻译**

对于顽固分支预执行（DBP），A-stream 保留并执行目标分支，却直接跳过它的 CD 区域；随后遇到 branch set 中的每个 CIDD 分支时，将其丢弃并跳过相应 CD 区域。因此目标分支可以在 A-stream 中提前求出真实方向，而 A-stream 不必沿其某条路径执行；计算出的谓词/方向则作为高精度预测交给 R-stream。

**English**

For Delinquent Load Prefetching (DLP), the A-stream converts the delinquent load into a non-binding prefetch, discards each branch in the load's branch set, and skips each associated CD region. The R-stream fills in missing local control-flow with its own prediction and execution. Global A/R control-flow correspondence still holds despite localized gaps.

**中文翻译**

对于顽固加载预取（DLP），A-stream 把目标 load 转成非绑定预取；对于该 load 的 branch set 中的每个分支，则丢弃分支并跳过其 CD 区域。缺失的局部控制流由 R-stream 自己预测并执行来补全。尽管两个流之间出现局部空洞，全局控制流仍保持一一对应。

**English**

Instruction removal now resembles ordinary branching: Slipstream 2.0 skips whole CD regions by using “branch-to-reconvergent-point” instead of “branch-to-taken-target,” rather than pruning arbitrary individual instructions.

**中文翻译**

这种指令删除在硬件上很像普通跳转：Slipstream 2.0 不需要任意删除零散指令，而是把“跳到 taken target”改成“直接跳到 reconvergent point”，一次略过整个 CD 区域。

**English**

The design also includes a hardware mechanism to enable or disable Slipstream 2.0 in profitable or unprofitable phases, making it a microarchitectural turbo-boost mode.

**中文翻译**

设计还加入纯硬件启停机制：在有收益的程序阶段启用 Slipstream 2.0，在无收益阶段关闭，使其成为一种微体系结构层面的“睿频加速模式”。

---

# 3. Slipstream Processor 2.0 Microarchitecture / 微体系结构

**English**

The A-stream and R-stream run on two cores. Each core has private L1 instruction and data caches, while L2 and L3 are shared. The A-stream's L1 data cache is speculative: evicted dirty blocks are discarded instead of written back, and dirty blocks are invalidated when the A-stream is restarted.

**中文翻译**

A-stream 与 R-stream 运行在两个核心上。每个核心拥有私有 L1 指令与数据 Cache，L2/L3 则共享。A-stream 的 L1 数据 Cache 是推测性的：被逐出的脏块不会写回；A-stream 回滚时，相关脏块被无效化。这样 A-stream 的错误写入不会污染正式架构状态。

**English**

A Delay Buffer communicates pre-executed branch outcomes from A-stream to R-stream. The IR-detector observes retired instructions, discovers what should be removed in future instances, and trains the IR-predictor. The IR-predictor performs actual pruning during A-stream instruction fetch.

**中文翻译**

Delay Buffer 把 A-stream 提前执行得到的分支结果传给 R-stream。IR-detector 观察退休指令流，学习以后应删除哪些指令并训练 IR-predictor；IR-predictor 位于 A-stream 取指端，在未来动态实例出现时真正执行删除或跳过操作。

## 3.1 IR-detector：检测什么应该删除

### 3.1.1 Identify Reconvergent Points / 识别汇合点

**English**

The Active Reconvergence Table (ART) studies one static branch at a time over multiple dynamic instances. It compares PCs retired after each instance, gradually gaining confidence in an inferred reconvergent point. Once confidence saturates, the point is installed in the Reconvergence Predictor Table (RPT), and ART moves to another branch.

**中文翻译**

ART（活动汇合表）每次分析一个静态分支，并跨多个动态实例比较该分支之后退休的 PC，从重复出现的位置中推断汇合点，逐渐提高置信度。置信度饱和后，将结果写入 RPT（汇合点预测表），ART 再转去分析其他分支。

**English**

RPT may retain up to three candidate reconvergent points per branch and returns the highest-confidence one. This filters a statically “truer” but dynamically rare distant reconvergence. ART plus RPT costs 1.1KB.

**中文翻译**

RPT 可为每个分支保留最多三个候选汇合点，并返回置信度最高者。这样可以过滤掉“从静态控制流看更彻底、但动态执行中很少到达”的远端汇合点。ART 与 RPT 合计占用 1.1KB。

### 3.1.2 Identify Delinquent Branches/Loads / 识别顽固分支与加载

**English**

The Branch/Load Classifier (BLC), indexed and tagged by PC, has a branch/load bit and a 16-bit misprediction/miss counter. Execution is divided into 500K-cycle epochs. Counters are cleared at epoch start and incremented for each branch misprediction or L2 load miss.

**中文翻译**

BLC（分支/加载分类器）用 PC 索引和标记，每项保存“分支或 load”类型位，以及 16 位的错误预测/Miss 计数器。运行被划分为每段 50 万周期的 epoch；每个 epoch 开始清零计数器，分支每次误预测或 load 每次 L2 Miss 时递增。

**English**

BLC-Max incrementally maintains the top eight delinquent branches/loads, avoiding a serial scan of the whole BLC at epoch end. Those eight are queued for branch-set analysis during the next epoch. A 128-entry BLC plus BLC-Max costs 0.8KB.

**中文翻译**

BLC-Max 增量维护当前最顽固的八个 branch/load，避免 epoch 结束时串行扫描整个 BLC。epoch 结束后，这八项进入队列，在下一个 epoch 逐项进行 branch-set 分析。128 项 BLC 与 BLC-Max 合计占用 0.8KB。

### 3.1.3 Branch Set Analysis / 分支集合分析

**English**

The Branch Set Buffer (BSB) learns one branch set at a time. It stores the target PC, reconvergent PC for a branch, up to 32 CIDD branch PCs, a CIDI bit, and a confidence counter. A delinquent branch begins with CIDI=1; if it is later found to depend on itself, the bit is cleared and it is not pre-executable.

**中文翻译**

BSB（分支集合缓冲区）每次学习一个目标的 branch set。它保存目标 PC、目标是分支时的汇合 PC、最多 32 个 CIDD 分支 PC、一个 CIDI 位以及置信度计数器。分析 delinquent branch 时，CIDI 初始为 1；如果后来发现它通过前向依赖回到自身，便清零该位，判定它不可预执行。

**English**

The Data Dependence Tracker (DDT) has one poison bit per logical register. At each target instance it is flash-cleared. For a target branch, every retired instruction in its CD region poisons its destination register. After reconvergence, control-independent instructions propagate poison from sources to destinations. A control-independent branch reading poisoned data is CIDD and is inserted into the branch set.

**中文翻译**

DDT（数据依赖跟踪器）为每个逻辑寄存器维护一个 poison 位。每次目标动态实例开始分析时全部快速清零。对于目标分支，其 CD 区域内退休指令写入的目的寄存器都会被“染毒”；到达汇合点后，控制独立指令再把毒化从源寄存器传播到目的寄存器。任何读取了毒化值的控制独立分支，就是 CIDD 分支并被加入 branch set。

**English**

When a CIDD branch is found, poisoning temporarily expands through that branch's CD region as well. This discovers transitive CIDD branches: branches that do not directly depend on the original target but depend on another CIDD branch. For a target load, the procedure is the same except the load itself has no CD region.

**中文翻译**

发现 CIDD 分支后，毒化还会临时扩展进该分支自己的 CD 区域，从而继续识别传递性 CIDD 分支：它们不直接依赖原目标，却依赖其他 CIDD 分支。目标若是 load，流程相同，只是 load 自身没有 CD 区域，毒化从它的目的寄存器及后续 CIDD 分支展开。

**English**

Analysis repeats across multiple dynamic instances to explore different paths. When confidence saturates, BSB trains the IR-predictor with the target's CIDI status and all valid CIDD branches. BSB plus DDT costs 0.1KB.

**中文翻译**

该分析跨多个动态实例重复，以探索尽可能多的路径。当置信度饱和时，BSB 把目标的 CIDI 状态以及所有有效 CIDD 分支写入 IR-predictor。BSB 与 DDT 合计只需 0.1KB。

## 3.2 IR-predictor：取指阶段如何行动

**English**

The A-stream fetch unit indexes the IR-predictor by PC. A hit on a load converts it to a non-binding prefetch. A hit on a CIDI branch marks it for pre-execution and redirects fetch to its reconvergent PC; resolving it does not squash A-stream. A hit on a CIDD branch discards it and redirects fetch to its reconvergent PC.

**中文翻译**

A-stream 取指单元用 PC 查询 IR-predictor。命中 load 项时，将其改成非绑定预取；命中 CIDI 分支时，保留分支用于预执行，同时把下一取指地址重定向到汇合 PC，该分支无论得到什么方向都不会清空 A-stream；命中 CIDD 分支时，直接丢弃该分支并跳到它的汇合 PC。

**English**

A misprediction/miss count guides replacement of the least delinquent entry. The paper uses a 128-entry, 1.2KB IR-predictor.

**中文翻译**

每项还保存误预测/Miss 计数，在空间满时优先替换最不顽固的一项。论文使用 128 项、总计 1.2KB 的 IR-predictor。

## 3.3 Delay Buffer / 延迟缓冲区

**English**

A-stream branch outcomes are passed to R-stream through the Delay Buffer, overriding R-stream's predictor. If an outcome is wrong, R-stream squashes and restarts A-stream. Each entry uses three logical bits: whether A-stream executed or discarded the branch, its outcome if executed, and whether its CD region was skipped.

**中文翻译**

A-stream 的分支结果经 Delay Buffer 传给 R-stream，并覆盖 R-stream 自己的分支预测。如果这个结果后来被证明错误，R-stream 清空并重新启动 A-stream。每项逻辑上包含三位：A-stream 是执行还是丢弃该分支；若执行，其结果是什么；以及 A-stream 是否跳过了该分支的 CD 区域。

**English**

Encoding 1x0 means normally executed with its CD region; 1x1 means pre-executed while its CD region was skipped; 0-1 means the CIDD branch was discarded. In the latter two cases R-stream may temporarily use its own predictor for branches in the local gap until reconvergence. If the Delay Buffer fills before R-stream reaches reconvergence, A-stream is restarted.

**中文翻译**

编码 `1x0` 表示分支及其 CD 区域正常执行，`x` 为方向；`1x1` 表示分支被预执行但 CD 区域被跳过；`0-1` 表示 CIDD 分支被丢弃。后两种情况会在两个流之间产生局部空洞，R-stream 到达汇合点前可暂时使用自己的预测器。如果 R-stream 尚未汇合而 Delay Buffer 已满，则必须重启 A-stream。

**English**

The paper uses 256 entries. Storage is 0.1KB if R-stream accesses RPT for reconvergent PCs, or about 1KB if each entry carries its reconvergent PC. Evaluation assumes the conservative 1KB design.

**中文翻译**

论文使用 256 项。如果 R-stream 可直接访问 RPT 获取汇合 PC，只需约 0.1KB；如果每个 Delay Buffer 项自己携带汇合 PC，则约为 1KB。评估采用较保守的 1KB 方案。

## 3.4 Proactive DLP 与 Reactive DLP

**English**

Proactive-DLP immediately converts a delinquent load to a prefetch at fetch, and classes all load-dependent branches as CIDD. This can unnecessarily block DBP of a branch that would have been pre-executable when the load hits.

**中文翻译**

主动式 DLP 在取指时立刻把顽固 load 转为预取，并把所有依赖该 load 的分支归为 CIDD。这带来一个问题：若 load 实际命中，某个依赖分支本来可能正确得到输入并适合 DBP，却因为提前将 load 非绑定化而失去预执行机会。

**English**

Reactive-DLP delays conversion until retirement: a load becomes a prefetch only if it is unresolved when reaching the ROB head. When training IR-predictor, existing CIDI status of dependent branches is preserved, allowing DBP to be attempted.

**中文翻译**

反应式 DLP 将转换推迟到退休：只有 load 到达 ROB 头部仍未完成时，才把它转成预取。在训练 IR-predictor 时，已经存在的依赖分支不会被强行从 CIDI 降为 CIDD，因此仍可尝试 DBP。

**English**

If the load ultimately misses and is converted, poison propagation from its destination detects whether the would-be CIDI branch actually depended on it. Just before writing the branch's Delay Buffer entry, that branch is dynamically downgraded to CIDD because it did not reliably pre-execute.

**中文翻译**

如果 load 最终确实 Miss 并被转为预取，硬件从 load 的目的寄存器开始传播 poison，判断原先计划作为 CIDI 预执行的分支是否真的依赖这个无效值。在把分支结果写入 Delay Buffer 前，若其源操作数被毒化，就动态将其降级为 CIDD，因为这次分支结果并不可靠。

## 3.5 Storage Cost / 存储开销

| 结构 | 组成 | 开销 |
|---|---|---:|
| IR-detector | BLC/BLC-Max + ART/RPT + BSB/DDT | 2.0KB |
| IR-predictor | 128 项 | 1.2KB |
| Delay Buffer | 256 项，含汇合 PC | 1.0KB |
| **合计** |  | **4.2KB** |

---

# 4. Evaluation / 实验方法

**English**

The authors compiled 25 SPEC CPU 2006/2017 benchmarks to RISC-V and selected their top-weighted 100-million-instruction SimPoints. A detailed cycle-level out-of-order simulator models either one baseline core or two cores operating as Slipstream 2.0, original Slipstream, or DCE.

**中文翻译**

作者把 25 个 SPEC CPU 2006/2017 benchmark 编译到 RISC-V，并为每个程序选择权重最高的 1 亿指令 SimPoint。详细的周期级乱序模拟器既可配置为单个基线核心，也可配置为双核 Slipstream 2.0、原始 Slipstream 或 DCE。

**English**

Each core is roughly configured after Intel Skylake: 4-wide fetch/retire, 8-wide issue/execute, 224-entry ROB, 32KB private L1 I/D caches, shared 256KB L2 and 8MB L3, 250-cycle DRAM, a 64KB TAGE-SC-L predictor, and a 5.5KB VLDP prefetcher. McPAT at 22nm estimates energy.

**中文翻译**

每个核心大致参照 Intel Skylake：每周期取指/退休 4 条、发射/执行 8 条、224 项 ROB、私有 32KB L1 I/D Cache、共享 256KB L2 与 8MB L3、DRAM 延迟 250 周期，并配备 64KB TAGE-SC-L 分支预测器和 5.5KB VLDP 预取器。能耗使用 22nm 配置的 McPAT 估算。

---

# 5. Results / 实验结果

## 5.1 Slipstream 2.0 对比基线

**English**

Figure 5 compares DBP-only, DLP-only, and combined DBP+DLP against the baseline, alongside perfect branch prediction and/or a perfect data cache. Detailed results focus on eight benchmarks for which automatic turbo boost enables Slipstream 2.0 for almost the whole SimPoint.

**中文翻译**

图 5 比较仅 DBP、仅 DLP 和 DBP+DLP 联合配置相对基线的加速，同时给出完美分支预测、完美数据 Cache 及二者皆完美的上界。详细结果集中在八个几乎全程被自动 turbo-boost 启用 Slipstream 2.0 的 benchmark 上。

### DBP 结果

**English**

Bzip2, astar, hmmer, and mcf have high branch MPKI and gain 17%-28% from DBP. A gap remains versus perfect prediction because not all mispredictions are pre-executable. In astar, 53% are pre-executed CIDI branches, 29% are CIDI branches nested inside skipped CD regions and therefore resolved locally in R-stream, and 18% are self-dependent CIDD branches that cannot be pre-executed.

**中文翻译**

Bzip2、astar、hmmer 和 mcf 的分支 MPKI 较高，DBP 带来 17%-28% 加速。它们与完美预测仍有差距，因为不是所有误预测分支都能预执行。以 astar 为例：53% 来自成功预执行的 CIDI 分支；29% 虽然也是 CIDI，却嵌套在另一个被跳过的 CD 区域中，只能在 R-stream 局部解析；剩余 18% 是存在自身依赖的 CIDD 分支，无法预执行。

**English**

The authors argue that helper threads generally cannot solve the fundamentally serial case where a branch's next dynamic instance depends on its previous instance.

**中文翻译**

作者认为，“下一动态实例依赖上一实例”的分支具有本质串行性，一般的辅助线程预执行也无法解决。这不是 Slipstream 2.0 某个小部件不够强，而是可利用并行性本身不存在。

### DLP 结果

**English**

Libquantum, lbm, omnetpp, mcf, and bwaves primarily suffer L2/L3 misses and gain between 13% and 2.9x from DLP. Mcf is notable because many mispredicted branches depend on cache-missed loads. Slipstream 2.0 insulates A-stream from those mispredictions when they resolve locally in R-stream; without this property, mcf speedup falls from 1.54 to about 1.35, as in DCE.

**中文翻译**

Libquantum、lbm、omnetpp、mcf 和 bwaves 主要受 L2/L3 Miss 限制，DLP 带来 13% 到 2.9 倍的加速。Mcf 特别重要，因为许多误预测分支依赖 Cache Miss load。Slipstream 2.0 让这些分支只在 R-stream 局部解析而不重启 A-stream；如果没有这项隔离能力，mcf 的加速比会由 1.54 降至约 1.35，与 DCE 接近。

### DBP + DLP

**English**

Combining DBP with reactive-DLP matches or exceeds either technique alone for every reported benchmark and achieves a geometric-mean speedup of 67% over baseline.

**中文翻译**

DBP 与反应式 DLP 组合后，在所有报告的 benchmark 上都不弱于其中任一单独技术，相对基线获得 67% 的几何平均加速。

## 5.2 对比 Slipstream 1.0 与 DCE

**English**

Original Slipstream removes very few instructions in high-branch-MPKI programs. For astar, A-stream retirement is reduced by only 4%, while A-stream still pays every misprediction penalty. Consequently it gives no speedup on high-MPKI benchmarks. DBP instead pre-executes CIDI branches and skips their CD regions.

**中文翻译**

在高分支 MPKI 程序中，原始 Slipstream 删除的指令很少。例如 astar 的 A-stream 退休指令数只减少 4%，却仍承担所有分支误预测惩罚，因此几乎没有加速。DBP 则直接识别并预执行 CIDI 分支，同时略过其 CD 区域。

**English**

DCE and DLP both convert delinquent loads into prefetches in A-stream. DCE leaves their forward control-flow slices and relies on dependent branches being correctly predicted; a misprediction rolls back A-stream and exposes the prefetch latency. DLP removes those dependent branches and CD regions from A-stream and resolves them locally in R-stream.

**中文翻译**

DCE 与 DLP 都在 A-stream 中把顽固 load 转为预取。区别是 DCE 保留 load 的前向控制流切片，并寄希望于依赖分支预测正确；一旦误预测，A-stream 回滚，预取延迟重新暴露。DLP 则从 A-stream 中删掉这些依赖分支及其 CD 区域，让它们在 R-stream 内局部解析。

**English**

DBP+DLP is 20%-30% faster than DCE on bzip2, astar, and mcf. For applications with few branch mispredictions it stays within 5% of DCE. Overall it achieves a 12% geometric-mean speedup over DCE.

**中文翻译**

在 bzip2、astar 和 mcf 上，DBP+DLP 比 DCE 多获得 20%-30% 的性能；在误预测很少的程序上，它与 DCE 相差不超过 5%。总体上，DBP+DLP 相对 DCE 的几何平均加速为 12%。

## 5.3 Microarchitectural Turbo Boost

**English**

The enable/disable heuristic checks whether MPKI of DBP-classed branches or DLP-classed loads exceeds thresholds in each epoch. Eight benchmarks enable A-stream almost throughout and benefit. Seventeen disable it for at least 60% of execution because they have low MPKI, are limited by true data dependencies, have only short profitable phases, or have high MPKI mainly from non-pre-executable branches.

**中文翻译**

启停启发式算法在每个 epoch 检查 DBP 类分支或 DLP 类 load 的 MPKI 是否超过阈值。八个 benchmark 几乎全程启用 A-stream 并得到加速；另有十七个在至少 60% 的时间里关闭它，原因包括 MPKI 很低、受真实数据依赖限制、只有局部阶段有收益，或者高 MPKI 主要来自不可预执行的分支。

## 5.4 Energy / 能耗

**English**

Despite using two cores, Slipstream 2.0 consumes 4% less energy on average than the one-core baseline and reduces EDP by 43%. Redundant execution raises dynamic energy, but shorter execution lowers static energy by more. Turbo boost also avoids redundant activity in unprofitable phases.

**中文翻译**

尽管使用两个核心，Slipstream 2.0 的平均总能耗仍比单核基线低 4%，EDP 下降 43%。冗余执行增加了动态能耗，但缩短运行时间带来的静态能耗下降更大；Turbo boost 还避免了在无收益阶段浪费双核能量。

---

# 6. Summary and Future Work / 总结与未来工作

**English**

Slipstream 2.0 is a hardware-only leader-follower pre-execution architecture that targets both branches and loads. Its key innovation is removing forward control-flow slices of pre-executable delinquent branches and delinquent loads from the leading thread. It adds only 4.2KB of storage.

**中文翻译**

Slipstream 2.0 是一种纯硬件、领导者-跟随者式预执行体系结构，同时针对分支和 load。其核心创新是从先行线程删除可预执行顽固分支与顽固 load 的前向控制流切片，新增存储开销仅为 4.2KB。

**English**

For auto-enabled SPEC SimPoints it achieves geometric-mean gains of 67%, 60%, and 12% over baseline, original Slipstream, and DCE. It reduces EDP by 43% and energy by 4% relative to baseline.

**中文翻译**

在自动启用该机制的 SPEC SimPoint 上，它相对基线、原始 Slipstream 和 DCE 分别实现 67%、60% 和 12% 的几何平均加速；相对基线，EDP 降低 43%，能耗降低 4%。

**English**

Only a certain class of delinquent branch can be effectively pre-executed. A branch is pre-executable only if it is not in its own forward control-flow slice. Further, if pre-executable branch B is control-dependent or CIDD on pre-executable branch A, both cannot be selected because B lies in A's forward slice. Non-pre-executable branches therefore remain a major open problem.

**中文翻译**

只有特定类别的顽固分支能够有效预执行：它不能位于自己的前向控制流切片中。更进一步，如果可预执行分支 B 对可预执行分支 A 控制依赖或属于 A 的 CIDD 分支，那么二者也不能同时被选中，因为 B 位于 A 的前向切片内。因此，不可预执行分支仍是重要的开放问题。

---

# 7. 论文重点总结

## 一句话总结

> Slipstream 2.0 用一个删减后的 A-stream 跑在完整 R-stream 前面；它通过自动学习 branch set，跳过 delinquent branch/load 的前向控制流影响，从而既能提前得到难预测分支的方向，也能把长延迟 load 变成预取，同时避免“load Miss 后的分支误预测”把先行流拖回去。

## 五个必须记住的贡献

1. **研究对象从后向切片转向前向控制流切片。** 不是只问“计算目标需要谁”，而是问“目标会影响后面哪些控制流”，并把这些影响从 A-stream 移除。
2. **提出 branch set。** 它同时充当“分支能否预执行”的判据，以及“应删除哪些前向控制流”的描述。
3. **DBP 与 DLP 统一。** DBP 提前解析分支；DLP 把顽固 load 变成非绑定预取；二者共享 branch-set 分析和 A/R 双流框架。
4. **解决 DCE 的关键失败情况。** 当 Cache Miss load 喂给误预测分支时，A-stream 不再因 R-stream 的局部分支恢复而重启。
5. **纯硬件、低元数据开销、按阶段启停。** 新增结构合计 4.2KB，并通过 turbo-boost 只在值得时启用第二核心。

## 最核心的判断公式

```text
目标分支 ∉ 它自己的 branch set
              ↓
目标分支没有跨动态实例的自身数据依赖
              ↓
目标分支可以 pre-execute
```

反过来：

```text
目标分支 ∈ 它自己的 branch set
              ↓
第 i+1 次分支依赖第 i 次分支路径产生的数据
              ↓
存在 loop-carried / self dependence
              ↓
不能通过并行预执行跑到前面
```

## 与上一篇 Speculative Precomputation 的关系

| 维度 | Speculative Precomputation | Slipstream 2.0 |
|---|---|---|
| 线程组织 | 主线程不断触发短 p-slice；可链式派生 | 长期存在的 A-stream + R-stream |
| 核心切片 | delinquent load 的 backward slice | branch/load 的 forward control-flow slice |
| 主要目标 | 长距离预取 load | branch 预执行 + load 预取 |
| 对齐方式 | Trigger、LIB、PSQ 管理动态辅助任务 | A/R 全局控制流一一对应，Delay Buffer 传结果 |
| 主要难点 | 及时启动、live-in 传递、跑多远 | 判断可预执行性、找 branch set、维护局部控制流空洞 |

## 论文的局限性

- 结果来自周期级模拟器和代表性 SimPoint，不是实际芯片测量。
- 使用两个核心提升一个线程性能，吞吐型工作负载下机会成本可能很高。
- 不可预执行的自依赖分支仍无法解决。
- 两个可预执行分支若存在控制/CIDD 关系，也不能同时选择。
- 汇合点和 branch set 都是动态学习的“可能结果”，错误或覆盖不足仍会影响收益。
- 论文报告的 67% 是在自动机制几乎全程启用的 8 个 SimPoint 上的几何平均值，不能理解为所有 25 个程序都普遍加速 67%。

---

# 8. 建议精读顺序

1. 先彻底理解 Figure 2：目标分支、CD region、CIDD branch、branch set、reconvergent point。
2. 再读 Section II-A-3 的 forward poisoning；这是论文算法核心。
3. 接着读 IR-predictor 的三种命中动作：load、CIDI branch、CIDD branch。
4. 然后读 Delay Buffer，弄清为什么 A/R-stream 仍然能对齐。
5. 最后用 mcf 对比 DLP 与 DCE，理解本文真正解决的 `load miss -> mispredicted branch` 问题。

下一次精读最适合从 **Figure 2 与 branch set 的构造**开始，因为后面的 IR-detector、DBP、DLP 都建立在这个概念之上。
