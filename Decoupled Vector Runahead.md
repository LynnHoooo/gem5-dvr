# Decoupled Vector Runahead

> 解耦向量前瞻执行

Ajeya Naithani Ghent University Belgium

Jaime Roelandts Ghent University Belgium

Sam Ainsworth University of Edinburgh United Kingdom

Timothy M. Jones University of Cambridge United Kingdom

Lieven Eeckhout Ghent University Belgium

## ABSTRACT / 摘要

We present Decoupled Vector Runahead (DVR), an in-core prefetching technique, executing separately to the main application thread, that exploits massive amounts of memory-level parallelism to improve the performance of applications featuring indirect memory accesses. DVR dynamically infers loop bounds at run-time, recognizing striding loads, and vectorizing subsequent instructions that are part of an indirect chain. It proactively issues memory accesses for the resulting loads far into the future, even when the out-of-order core has not yet stalled, bringing their data into the L1 cache, and thus providing timely prefetches for the main thread. DVR can adjust the degree of vectorization at run-time, vectorize the same chain of indirect memory accesses across multiple invocations of an inner loop, and efficiently handle branch divergence along the vectorized chain. DVR runs as an on-demand, speculative, in-order, lightweight hardware subthread alongside the main thread within the core and incurs a minimal hardware overhead of only 1139 bytes. Relative to a large superscalar 5-wide out-of-order baseline and Vector Runahead — a recent microarchitectural technique to accelerate indirect memory accesses on out-of-order processors — DVR delivers 2.4× and 2× higher performance, respectively, for a set of graph analytics, database, and HPC workloads.

> **中文翻译：** 我们提出了解耦向量 Runahead (DVR)，这是一种核心预取技术，单独执行到主应用程序线程，它利用大量内存级并行性来提高具有间接内存访问功能的应用程序的性能。 DVR 在运行时动态推断循环边界，识别步幅负载，并对属于间接链一部分的后续指令进行向量化。即使乱序核心尚未停止，它也会主动为将来的结果负载发出内存访问，将其数据放入 L1 缓存，从而为主线程提供及时的预取。 DVR 可​​以在运行时调整向量化程度，对内部循环的多次调用之间的同一间接内存访问链进行向量化，并有效地处理向量化链上的分支分歧。 DVR 作为按需、推测、有序、轻量级硬件子线程与核心内的主线程一起运行，并且产生的硬件开销仅为 1139 字节。相对于大型超标量 5 宽乱序基线和 Vector Runahead（一种最新的微架构技术，用于加速乱序处理器上的间接内存访问），DVR 为一组图分析、数据库和 HPC 工作负载分别提供了 2.4 倍和 2 倍的更高性能。

## CCS CONCEPTS / CCS 概念

• **Computer systems organization** → **Superscalar architectures** ; _Single instruction, multiple data_ .

> **中文翻译：** • 计算机系统组织→超标量体系结构；单指令，多数据。

## KEYWORDS / 关键词

CPU microarchitecture, prefetching, runahead, speculative vectorization, graph processing

> **中文翻译：** CPU 微架构、预取、前瞻执行、推测向量化、图形处理

#### ACM Reference Format: / ACM参考格式：

Ajeya Naithani, Jaime Roelandts, Sam Ainsworth, Timothy M. Jones, and Lieven Eeckhout. 2023. Decoupled Vector Runahead. In _56th Annual IEEE/ACM International Symposium on Microarchitecture (MICRO ’23), October 28– November 01, 2023, Toronto, ON, Canada._ ACM, New York, NY, USA, 15 pages. https://doi.org/10.1145/3613424.3614255

> **中文翻译：** 阿杰亚·奈萨尼 (Ajeya Naithani)、杰米·罗兰茨 (Jaime Roelandts)、萨姆·安斯沃斯 (Sam Ainsworth)、蒂莫西·M·琼斯 (Timothy M. Jones) 和利文·埃克霍特 (Lieven Eeckhout)。 2023.解耦向量前瞻执行。第 56 届 IEEE/ACM 国际微架构研讨会 (MICRO ’23)，2023 年 10 月 28 日至 11 月 1 日，加拿大安大略省多伦多。 ACM，美国纽约州纽约市，15 页。 https://doi.org/10.1145/3613424.3614255

## 1 INTRODUCTION / 1 引言

Out-of-order cores are bigger than ever, with the latest processors featuring reorder buffers of many hundreds of entries [33]. And yet, although modern-day out-of-order (OoO) processors are given more than ample resources, and thus their out-of-order queueing resources are rarely filled to capacity, they are still memory-bound especially for workloads that feature chains of dependent memory accesses, or indirect memory accesses. One recent proposal, Vector Runahead [67, 68], presents a potential method for doing better. Rather than work-skipping as earlier runahead proposals do [29, 40, 62, 66] to keep uncovering memory-level parallelism, Vector Runahead reformulates the transient execution performed within runahead mode to be primarily based on loop-level parallelism, following independent groups of many different _dependent_ chains of memory accesses from future loop iterations in the program, and running them in a vectorized manner to reduce front-end and back-end pipeline resource requirements.

> **中文翻译：** 乱序内核比以往任何时候都更大，最新的处理器具有数百个条目的重排序缓冲区[33]。然而，尽管现代乱序 (OoO) 处理器被赋予了充足的资源，因此它们的乱序队列资源很少被填满，但它们仍然受到内存限制，特别是对于具有依赖内存访问链或间接内存访问功能的工作负载。最近的一项提议，Vector Runahead [67, 68]，提出了一种做得更好的潜在方法。 Vector Runahead 没有像早期的先行提案[29,40,62,66]那样跳过工作来不断发现内存级并行性，而是将先行模式中执行的瞬态执行重新制定为主要基于循环级并行性，遵循来自程序中未来循环迭代的许多不同依赖的内存访问链的独立组，并以向量化方式运行它们，以减少前端和后端管道资源需求。

Vector Runahead (VR) can successfully follow and prefetch the complex memory-access patterns in modern graph analytics, database and high-performance computing (HPC) workloads. However, like the underlying out-of-order core, even with a large reorder buffer (ROB), Vector Runahead is still memory-bound. Because the large reorder buffer rarely fills up, the resource starvation that triggers Vector Runahead rarely occurs, and so its benefits over even resource-bountiful out-of-order execution are not allowed to shine.

> **中文翻译：** Vector Runahead (VR) 可以成功跟踪和预取现代图分析、数据库和高性能计算 (HPC) 工作负载中的复杂内存访问模式。然而，与底层的乱序核心一样，即使具有较大的重排序缓冲区 (ROB)，Vector Runahead 仍然受内存限制。由于大型重排序缓冲区很少填满，因此触发 Vector Runahead 的资源匮乏很少发生，因此即使资源丰富的乱序执行也无法发挥其优势。

We propose _Decoupled Vector Runahead (DVR)_ , which innovates over prior runahead proposals in several key ways. First, it completely decouples the runahead process from the main computation thread, by running it within a lightweight, in-order subthread context of its own, allowing initiation even when the core is not stalled on a full ROB, and allowing the main thread to continue to make progress on its intended computation. Second, building on VR, it implements GPU-style divergence and reconvergence on the many dynamically generated ‘lanes’ produced from the many future loop iterations within the speculative runahead context. Third, it performs a _discovery mode_ within the main computation’s thread to precisely predict how many loops into the future will be accessed, to limit inaccurate prefetches. When it has too few locations to prefetch from discovery mode alone, it performs _nested vector runahead_ to generate inputs for many inner loop invocations from many different outer loop iterations simultaneously, which can then all be efficiently vectorized together to achieve extreme memory-level parallelism, even for workloads with complex data- and control-flow dependencies.

> **中文翻译：** 我们提出了解耦向量前瞻执行（DVR），它在几个关键方面对之前的前瞻执行建议进行了创新。首先，它将前瞻执行进程与主计算线程完全解耦，通过在其自己的轻量级、有序子线程上下文中运行它，即使核心没有在完整的 ROB 上停滞时也允许启动，并允许主线程继续在其预期计算上取得进展。其次，它以 VR 为基础，在推测性前瞻执行环境中的许多未来循环迭代产生的许多动态生成的“通道”上实现了 GPU 式的发散和再收敛。第三，它在主计算线程中执行发现模式，以精确预测未来将访问多少个循环，以限制不准确的预取。当它的位置太少而无法单独从发现模式预取时，它会执行嵌套向量提前运行，以同时从许多不同的外循环迭代生成许多内循环调用的输入，然后可以将所有输入有效地向量化在一起，以实现极端的内存级并行性，即使对于具有复杂数据和控制流依赖性的工作负载也是如此。

Decoupled Vector Runahead proactively prefetches cache-missing loads far in advance, meaning such loads do not sit in the reorder

> **中文翻译：** 解耦的 Vector Runahead 会提前很长时间主动预取缓存缺失的负载，这意味着此类负载不会出现在重新排序中

for(i=0; i<NUM_KEYS; i++) { C[hash(B[hash(A[i])])]++; }

### Figure 1: Example indirect memory access pattern [67]. / 图 1：间接内存访问模式示例 [67]。

buffer stalling commit or preventing branches from being resolved, let alone stall the reorder buffer entirely. Performance improves substantially as a result of its accurate, timely prefetches. DVR means runahead is no longer an alternative to very large instruction windows for out-of-order processors [66]. In fact, it is much better by offering huge performance benefits even in addition to such a large instruction window. Our simulation results using a broad set of graph analytics, database, and HPC workloads report that DVR yields 2.4× and 2× higher performance on average (and up to 6.4× and 5.2×) compared to a baseline OoO core (with a 350-entry ROB) and Vector Runahead, respectively. We further demonstrate that the performance boost DVR offers is maintained when increasing ROB sizes, in contrast to Vector Runahead, thanks to its high accuracy, high coverage and timeliness, when prefetching many future dependent load chains in parallel and decoupled from the main thread.

> **中文翻译：** 缓冲区停止提交或阻止分支被解析，更不用说完全停止重排序缓冲区了。由于其准确、及时的预取，性能得到显着提高。 DVR 意味着前瞻执行不再是乱序处理器的超大指令窗口的替代方案 [66]。事实上，即使除了如此大的指令窗口之外，它还提供巨大的性能优势，效果要好得多。我们使用大量图分析、数据库和 HPC 工作负载进行的模拟结果表明，与基准 OoO 核心（具有 350 个条目 ROB）和 Vector Runahead 相比，DVR 的平均性能分别提高了 2.4 倍和 2 倍（最高分别为 6.4 倍和 5.2 倍）。我们进一步证明，与 Vector Runahead 相比，在增加 ROB 大小时，DVR 提供的性能提升得以维持，这要归功于它的高精度、高覆盖率和及时性，在并行预取许多未来的依赖负载链并与主线程解耦时。

## 2 BACKGROUND / 2 背景

## 2.1 Runahead Execution / 2.1 前瞻执行

Runahead execution [29, 40, 62, 66] prefetches future memory accesses into on-chip caches after the _instruction window_ or _reorder buffer_ of an out-of-order core fills up and stalls with a memory access at the head of the buffer. To avoid a long-latency memory access from stalling the core, it will evict the instruction from its reorder buffer, but continue with instructions after it. While these instructions will no longer be strictly correct, and will be rolled back later, the prefetches generated as a result are accurate, as it speculatively pre-executes the application’s own future instruction stream. The processor stays in _runahead mode_ for its _runahead interval_ : the number of cycles from the full-ROB stall to the return of the long-latency memory access. Following this, it returns to normal (correct) execution mode.

> **中文翻译：** 超前执行[29,40,62,66]在乱序内核的指令窗口或重排序缓冲区填满并停止缓冲区头部的内存访问后，将未来的内存访问预取到片上缓存中。为了避免长时间延迟的内存访问导致内核停顿，它将从其重排序缓冲区中逐出该指令，但继续执行其后的指令。虽然这些指令将不再严格正确，并且稍后会回滚，但由此生成的预取是准确的，因为它推测性地预执行应用程序自己的未来指令流。处理器在其前瞻执行间隔内保持前瞻执行模式：从完全 ROB 停顿到长延迟内存访问返回的周期数。此后，它返回到正常（正确）执行模式。

Precise Runahead Execution (PRE) [69, 70] improves the performance of prior runahead techniques in three ways: (1) in runahead mode, it improves prefetch coverage by only executing chains of instructions that lead to full-ROB stalls, (2) it does not flush the reorder buffer when exiting runahead mode, therefore saving the penalty for flushing and refilling the pipeline, and (3) it can prefetch future memory accesses even for short runahead intervals. One key characteristic of all runahead techniques, including precise runahead, is that they depend on the processor front-end for delivering future instructions for the duration of a runahead interval. Consequently, the number of instructions executed in the runahead mode depends on the front-end width and runahead interval.

> **中文翻译：** 精确超前执行 (PRE) [69, 70] 通过三种方式提高了先前超前技术的性能：(1) 在前瞻执行模式下，它仅通过执行导致完全 ROB 停顿的指令链来提高预取覆盖率，(2) 在退出前瞻执行模式时，它不会刷新重排序缓冲区，因此节省了刷新和重新填充管道的代价，以及 (3) 即使前瞻执行间隔很短，它也可以预取未来的内存访问。所有前瞻执行技术（包括精确前瞻执行）的一个关键特征是，它们依赖于处理器前端在前瞻执行间隔期间传递未来的指令。因此，在前瞻执行模式下执行的指令数量取决于前端宽度和前瞻执行间隔。

## 2.2 Indirect Memory Accesses / 2.2 间接访存

Many modern applications feature dependent memory accesses with complex address-calculation patterns and multiple levels of indirection. A simple example of such patterns is shown in Figure 1.

> **中文翻译：** 许多现代应用程序都具有具有复杂地址计算模式和多个间接级别的相关内存访问功能。图 1 显示了此类模式的一个简单示例。

Here, array _A_ is accessed sequentially. However, the index to access array _B_ is calculated by hashing the value at a particular index of _A_ , and the index to array _C_ is calculated by hashing the access to _B_ . That is, accesses to _C_ depend on accesses to _B_ , which in turn depend on accesses to _A_ . Accesses to _B_ and _C_ are termed the first and second levels of indirect memory accesses, respectively, and the chain of instructions between the access of array _A_ and the access to array _C_ is termed the _indirect chain_ .

> **中文翻译：** 这里，数组A被顺序访问。但是，访问数组 B 的索引是通过对 A 的特定索引处的值进行散列计算的，而数组 C 的索引是通过对 B 的访问进行散列​​计算的。也就是说，对 C 的访问取决于对 B 的访问，而 B 又取决于对 A 的访问。对 B 和 C 的访问分别称为第一级和第二级间接存储器访问，对数组 A 的访问和对数组 C 的访问之间的指令链称为间接链。

For workloads with indirect memory accesses, traditional runahead techniques fail to prefetch the majority of future memory accesses for two main reasons. First, even in the presence of a stride prefetcher, PRE cannot prefetch memory accesses beyond the first level of indirection [67]. For the example in Figure 1, depending on the work-skipping technique, the inputs to array C will either be invalidated [29], or fail to return before runahead terminates [69, 70]. Second, even for the first level of indirect memory accesses, the number of instructions (or the number of iterations of the loop) covered in runahead mode is limited by the width of the processor front-end and the runahead interval.

> **中文翻译：** 对于具有间接内存访问的工作负载，传统的前瞻执行技术无法预取未来的大部分内存访问，主要原因有两个。首先，即使存在跨步预取器，PRE 也无法预取超出第一级间接寻址的内存访问 [67]。对于图 1 中的示例，根据工作跳过技术，数组 C 的输入将无效 [29]，或者在前瞻执行终止之前无法返回 [69, 70]。其次，即使对于第一级间接存储器访问，前瞻执行模式中覆盖的指令数量（或循环迭代次数）也受到处理器前端的宽度和前瞻执行间隔的限制。

## 2.3 Vector Runahead / 2.3 向量前瞻执行

Vector Runahead (VR) [67] reinvents runahead execution — and alleviates the previously mentioned shortcomings — in three ways. First, it automatically generates instructions at different indices of an indirect chain, therefore eliminating the dependence of prior runahead techniques on the processor front-end for instruction supply in runahead mode. It then reorders those instructions such that many of them at a particular offset can be executed in parallel. This leads to all the load instructions at a particular offset being issued to the memory system simultaneously. Consequently, instead of waiting for one memory access to return — as typical runahead techniques like PRE do — the core waits for many memory accesses at the same time. Second, it groups a large number of reordered scalar instructions into vectors; this reduces the pressure on backend resources, like the issue queue and execution units, to process instructions. Third, VR performs _delayed termination_ , which only leaves runahead once memory accesses for an entire indirect chain have been generated, because it is faster at generating memory-level parallelism (MLP) than normal-mode execution.

> **中文翻译：** Vector Runahead (VR) [67] 通过三种方式重新发明了提前执行，并减轻了前面提到的缺点。首先，它自动在间接链的不同索引处生成指令，因此消除了现有前瞻执行技术对处理器前端在前瞻执行模式下提供指令的依赖。然后，它对这些指令重新排序，以便可以并行执行特定偏移处的许多指令。这导致特定偏移处的所有加载指令同时发送到存储系统。因此，核心不会像 PRE 等典型的前瞻执行技术那样等待一次内存访问返回，而是会同时等待许多内存访问。其次，它将大量重新排序的标量指令分组为向量；这减少了后端资源（例如发出队列和执行单元）处理指令的压力。第三，VR 执行延迟终止，只有在生成整个间接链的内存访问后才会提前运行，因为它生成内存级并行性 (MLP) 的速度比正常模式执行更快。

In VR, the core enters runahead mode after a full-ROB stall. The process of reinterpreting scalars as vectors, or _speculative vectorization_ , begins when the core encounters a striding load marking the beginning of an indirect chain. The processor vectorizes the stride load and its dependents to generate prefetches.

> **中文翻译：** 在 VR 中，核心在完全 ROB 停顿后进入前瞻执行模式。当核心遇到标记间接链开始的步幅负载时，将标量重新解释为向量或推测向量化的过程就开始了。处理器对步幅负载及其相关项进行向量化以生成预取。

For the example in Figure 1, VR simultaneously generates accesses for multiple iterations of A (for example, from _i=0_ to _63_ ) by reinterpreting the scalar load instruction accessing _A[0]_ to a set of vector-gather instructions that access _A[0–63]_ . Once this first set of loads returns, it begins the vectorization of the arithmetic instructions comprising the hash() function to calculate the indices _B[hash(A[0–63])]_ . The gather instruction accessing _B[...]_ accesses many different cachelines due to the indirect nature of accesses to array _B_ , and therefore, instead of waiting for one memory access, the processor concurrently waits for 64 non-contiguous memory accesses. When these return, it generates memory accesses for all

> **中文翻译：** 对于图 1 中的示例，VR 通过将访问 A[0] 的标量加载指令重新解释为访问 A[0–63] 的一组向量收集指令，同时生成对 A 的多次迭代的访问（例如，从 i=0 到 63 ）。一旦第一组负载返回，它就开始对包含 hash() 函数的算术指令进行向量化，以计算索引 B[hash(A[0–63])] 。由于访问数组 B 的间接性质，访问 B[...] 的收集指令会访问许多不同的缓存行，因此，处理器不是等待一次内存访问，而是同时等待 64 个非连续内存访问。当这些返回时，它会生成所有的内存访问

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0003-02.png)

**Figure 2: Performance of an OoO core and VR, normalized to a baseline 350-entry ROB OoO core (left axis), and processor stall time due to a full ROB (right axis), as a function of ROB size.** _The performance gain of VR diminishes with increasing ROB size, and for some benchmarks overall performance even decreases._

> **中文翻译：** 图 2：OoO 内核和 VR 的性能，标准化为基准 350 条目 ROB OoO 内核（左轴），以及由于完整 ROB 导致的处理器停顿时间（右轴），作为 ROB 大小的函数。 VR 的性能增益会随着 ROB 大小的增加而减弱，对于某些基准测试，整体性能甚至会下降。

64 non-contiguous accesses to _C_ . The processor then terminates runahead mode, as it has reached the last indirect load in the chain.

> **中文翻译：** 64 对 C 的非连续访问。然后，处理器终止前瞻执行模式，因为它已到达链中的最后一个间接加载。

## 3 MOTIVATION / 3 研究动机

While Vector Runahead [67] is the first runahead technique to target indirect memory accesses and deliver substantially higher performance than prior runahead techniques, it is limited by the following factors:

> **中文翻译：** 虽然 Vector Runahead [67] 是第一个以间接内存访问为目标的前瞻执行技术，并且比之前的前瞻执行技术提供了更高的性能，但它受到以下因素的限制：

**(1) Performance Boost Diminishes with Bigger ROBs.** Like all prior runahead techniques, VR waits for the reorder buffer to fill up. However, the size of the reorder buffer has consistently increased over recent years, and it therefore takes more cycles to fill up. As a result, the opportunity to enter runahead mode decreases with increasing ROB size, as reported in Figure 2. Indeed, processor stall time due to a full ROB reduces from 51% to 5% for an ROB size of 128 to 512 entries, respectively.

> **中文翻译：** (1) ROB 越大，性能提升就越小。与所有先前的前瞻执行技术一样，VR 等待重排序缓冲区填满。然而，近年来，重排序缓冲区的大小不断增加，因此需要更多的周期才能填满。因此，进入前瞻执行模式的机会随着 ROB 大小的增加而减少，如图 2 所示。事实上，对于 128 个条目到 512 个条目的 ROB 大小，由于完整 ROB 导致的处理器停顿时间分别从 51% 减少到 5%。

Reduced opportunity to enter runahead mode leads to a commensurate reduction in the performance boost VR offers. Figure 2 also reports performance for an OoO core and VR as a function of ROB size from 128 to 512 entries, normalized to our 350-entry ROB baseline OoO core (see Section 5 for the full experimental setup). While VR improves performance for all ROB sizes, and is faster than any out-of-order baseline no matter how small the ROB is, the performance benefit offered by VR diminishes with increasing ROB size. For some benchmarks this is so dramatic that _absolute performance actually decreases_ with increasing ROB size. This is particularly the case for sssp, as well as bc, bfs and cc to a lesser extent. A smaller ROB triggers VR more often, which is faster than OoO execution and thus enables prefetching further down the future instruction stream. Decoupling from a full-ROB stall has the opportunity to trigger vector-runahead execution more frequently and hence deliver higher performance.

> **中文翻译：** 进入超前模式的机会减少会导致 VR 提供的性能提升相应减少。图 2 还报告了 OoO 核心和 VR 的性能，作为从 128 到 512 个条目的 ROB 大小的函数，标准化为我们的 350 条目 ROB 基线 OoO 核心（有关完整实验设置，请参阅第 5 节）。虽然 VR 提高了所有 ROB 大小的性能，并且无论 ROB 有多小，都比任何乱序基线更快，但 VR 提供的性能优势会随着 ROB 大小的增加而减弱。对于某些基准测试来说，这是非常显着的，以至于绝对性能实际上随着 ROB 大小的增加而下降。对于 sssp 以及较小程度上的 bc、bfs 和 cc 来说尤其如此。较小的 ROB 会更频繁地触发 VR，这比 OoO 执行速度更快，因此可以在未来的指令流中进一步预取。与完全 ROB 停顿解耦有机会更频繁地触发向量超前执行，从而提供更高的性能。

_Key Insight #1: To maximize prefetching opportunity, VR must not wait for a full-ROB stall._

> **中文翻译：** 关键见解 1：为了最大化预取机会，VR 不得等待完全 ROB 停顿。

**(2) Delayed Termination Stalls Commit.** VR terminates runahead mode only after vectorizing the last load instruction in the indirect chain and generating prefetches for it. Meanwhile, it is likely that the load instruction that originally blocked the head of the ROB, and caused the ROB to fill up, has returned from memory.

> **中文翻译：** (2) 延迟终止停止提交。仅在向量化间接链中的最后一个加载指令并为其生成预取后，VR 才会终止前瞻执行模式。同时，最初阻塞ROB头部并导致ROB填满的加载指令很可能已从存储器返回。

Although the OoO core can now commit instructions from the ROB, the processor does not return to normal mode, so as to allow the vectorized chain to complete first. This delayed termination stalls the commit stage on average 7.1% (and up to 11.8%) of the total execution time in VR across our set of benchmarks. This is a missed opportunity for the main pipeline to progress.

> **中文翻译：** 尽管 OoO 核心现在可以提交来自 ROB 的指令，但处理器不会返回到正常模式，以便允许向量化链首先完成。在我们的一组基准测试中，这种延迟终止使提交阶段平均延迟了 VR 总执行时间的 7.1%（最高可达 11.8%）。这是主管道进展的错失良机。

Key Insight #2: The process of vectorization and generating prefetches in runahead mode under VR must be decoupled from the main pipeline, so that the main core can also make forward progress while prefetching along the speculatively vectorized indirect chain.

> **中文翻译：** 关键见解2：VR下以runahead模式进行向量化和生成预取的过程必须与主管道解耦，以便主核在沿着推测性向量化间接链进行预取的同时也能向前推进。

**(3) Cannot Adapt to Run-time Characteristics.** Vector Runahead attempts to generate as many gathers for each scalar load as possible. The goal is to achieve high memory-level parallelism by keeping all the miss status holding registers (MSHR) occupied by the outstanding memory accesses. However, this assumes that the workload’s induction-variable access, from which we spawn future dependent chains, continues to steadily increase far into the future. When we look at more complicated workloads, this assumption begins to falter, and yet they still exhibit memory-level parallelism.

> **中文翻译：** (3)无法适应运行时特性。 Vector Runahead 尝试为每个标量负载生成尽可能多的集合。目标是通过保持所有未命中状态保持寄存器（MSHR）被未完成的内存访问占用来实现高内存级并行性。然而，这假设工作负载的归纳变量访问（我们从中产生未来的依赖链）在未来继续稳步增长。当我们看到更复杂的工作负载时，这种假设开始动摇，但它们仍然表现出内存级并行性。

Algorithm 1: Breadth-first search. _There are two strides (at lines 4 and 8) from which we can start Vector Runahead, resulting in a chain length of 4 or 2 respectively, and a highly data-dependent branch at line 9._

> **中文翻译：** 算法1：广度优先搜索。我们可以从两个步幅（第 4 行和第 8 行）开始 Vector Runahead，分别产生 4 或 2 的链长度，以及第 9 行高度依赖数据的分支。

|**1**|Queue workList ={startNode}|
|---|---|
|**2**|Array visited[startNode] = true|
|**3 **|**while**_worklist_≠∅**do**|
|**4**|Vertex V = workList.pop()|
|**5**|Edge E1 = Vertices[V]|
|**6**|Edge E2 = Vertices[V+1]|
|**7**|**for**_Edge E=E1;E<E2;E++)_**do**|
|**8**|Vertex W = edgeTo[E]|
|**9**|**if** _!visited[W]_**then**|
|**10**|workList.push(W)|
|**11**|visited[W] = true|

**中文表格：**

|**1**|队列工作列表={startNode}|
|---|---|
|**2**|数组访问[startNode] = true|
|**3 **|while工作清单≠∅do|
|**4**|顶点 V = workList.pop()|
|**5**|边 E1 = 顶点[V]|
|**6**|边 E2 = 顶点[V+1]|
|**7**|forEdge E=E1;E<E2;E++)do|
|**8**|顶点 W = edgeTo[E]|
|**9**|if !visited[W]then|
|**10**|工作列表.push(W)|
|**11**|访问过[W] = true|

Breadth-first search is a widely used graph-traversal algorithm that is used both in its own right and also as a kernel for finding connected components, maximum flows by the Edmonds-Karp algorithm [31], betweenness centrality [16], and many more. Algorithm 1 shows pseudocode matching the behavior of both the top-down step of GAP [12] and Graph500 [6]. In this workload, there are two possible points from which we can start Vector Runahead (two striding loads) at lines 4 and 8. Typically we will wish to vectorize from the latter, as it is an inner loop and so the accesses will be more timely. However, the length of this inner loop will be extremely data-dependent: not just on the size of the graph, but also its structure. Often, the loop will be far shorter than the amount we wish to vectorize by, and so Vector Runahead will fetch a significant amount of data the true execution will never access, polluting the cache and wasting DRAM bandwidth.

> **中文翻译：** 广度优先搜索是一种广泛使用的图遍历算法，它既可以单独使用，也可以用作查找连接组件、Edmonds-Karp 算法 [31] 的最大流、介数中心性 [16] 等的内核。算法 1 显示了与 GAP [12] 和 Graph500 [6] 自上而下步骤的行为相匹配的伪代码。在此工作负载中，我们可以从第 4 行和第 8 行开始 Vector Runahead（两个跨步加载）的两个可能点。通常我们希望从后者进行向量化，因为它是一个内部循环，因此访问会更及时。然而，这个内部循环的长度将极大地依赖于数据：不仅取决于图的大小，还取决于它的结构。通常，循环会比我们希望向量化的量短得多，因此 Vector Runahead 将获取真正执行永远不会访问的大量数据，从而污染缓存并浪费 DRAM 带宽。

_Key Insight #3: VR needs to (i) learn the data-dependent, dynamic number of iterations of each loop it runs, to avoid fetching useless data, and (ii) update this each time it runs to respond to the latest run-time values._

> **中文翻译：** 关键见解 3：VR 需要 (i) 了解其运行的每个循环的数据相关的动态迭代次数，以避免获取无用的数据，以及 (ii) 每次运行时更新该数据以响应最新的运行时值。

**(4) Inability to Vectorize Multiple Invocations of the Same Loop.** If one iteration of a loop does not have enough loads to prefetch, the MLP exposed by VR is limited. It can begin the speculative vectorization of multiple invocations as they pass the main core but each time it only generates a small number of memory accesses, which are often generated by the core in the very near future anyway. To be able to fetch ahead far enough, VR must increase the degree of vectorization by discovering the correct values for multiple future versions of the same inner loop. In the breadth-first search example, this means we must be able to generate many stride accesses from line 4, and follow their dependencies through to line 8, in order to run not just many loads from within the loop, but many different versions of the inner loop from different outer loops simultaneously.

> **中文翻译：** (4) 无法对同一循环的多次调用进行向量化。如果循环的一次迭代没有足够的负载来预取，则 VR 公开的 MLP 就会受到限制。当多个调用通过主核心时，它可以开始对它们进行推测向量化，但每次它只生成少量的内存访问，而这些访问通常是由核心在不久的将来生成的。为了能够取得足够的进展，VR 必须通过发现同一内循环的多个未来版本的正确值来提高向量化程度。在广度优先搜索示例中，这意味着我们必须能够从第 4 行生成许多跨步访问，并跟踪它们的依赖关系直到第 8 行，以便不仅从循环内运行许多负载，而且同时从不同的外循环运行许多不同版本的内循环。

_Key Insight #4: VR needs to look ahead to many future iterations of the same loop, if a single loop is dynamically determined to be too small to saturate the memory system, by skipping ahead to discover inputs to the same code from different outer loop iterations that will execute in the near future._

> **中文翻译：** 关键见解 4：如果动态确定单个循环太小而无法使内存系统饱和，VR 需要预见同一循环的许多未来迭代，方法是跳过以发现来自将在不久的将来执行的不同外循环迭代的相同代码的输入。

**(5) Inability to Handle Control-Flow Divergence.** Vector Runahead follows the control flow of the first scalar-equivalent instruction in the set of vectorized lanes, invalidating lanes with control-flow divergence. In the breadth-first search example, this is fine provided that the first edge in the sequence does not end in a previously visited vertex. Otherwise, we fail to execute prefetches for the operations inside the _if_ -statement within the loop. In other workloads, such as betweenness centrality, there may be much broader divergence, with completely different memory accesses down each path.

> **中文翻译：** (5) 无法处理控制流发散。 Vector Runahead 遵循向量化通道集中第一个标量等效指令的控制流，使具有控制流分歧的通道无效。在广度优先搜索示例中，只要序列中的第一条边不以先前访问过的顶点结束，就可以了。否则，我们无法对循环内 if 语句内的操作执行预取。在其他工作负载中，例如介数中心性，可能存在更广泛的差异，每条路径上的内存访问完全不同。

Ideally, we should follow the true control flow of every single vector lane — and yet we still want to execute instructions as vectors whenever possible, getting the maximal use, and maximal parallelism, from each scalar-equivalent operation. To do this, we should take inspiration from GPUs, allowing threads to diverge and reconverge [54] when necessary.

> **中文翻译：** 理想情况下，我们应该遵循每个向量通道的真实控制流，但我们仍然希望尽可能将指令作为向量执行，从每个标量等效操作中获得最大的使用和最大的并行性。为此，我们应该从 GPU 中汲取灵感，允许线程在必要时发散和重新聚合 [54]。

_Key Insight #5: VR should remove the constraint of control-flow matching between lanes, by supporting full SIMT GPU-style divergence and reconvergence._

> **中文翻译：** 关键见解 5：VR 应通过支持完整的 SIMT GPU 式发散和再收敛来消除通道之间控制流匹配的限制。

## 4 DVR MICROARCHITECTURE / 4 DVR 微体系结构

Decoupled Vector Runahead overcomes the shortcomings listed in the previous section as follows. When the core discovers that it is executing a loop with dependent loads, based on a striding load that can be used to predict future loop iterations, a specialized _vector-runahead subthread_ is activated on the same core as the currently executing main thread. This subthread is dynamically generated to prefetch many memory accesses into the future, but without affecting the semantics of the main thread. The vector-runahead subthread runs alongside the main thread on the same core, much like how threads co-execute in simultaneous multithreading (SMT) [91], except that the subthread is microarchitecturally generated, transient (to prefetch into the cache rather than achieve real computation), speculative, reordered to achieve extremely high memory-level parallelism, and significantly simpler, i.e., the subthread executes in-order. The vector-runahead subthread is also closely related to simultaneous subordinate microthreading [20], which also aims at improving performance of the main thread. Whereas a subordinate microthread is written in microcode featuring specialized machine-specific instructions, the vector-runahead subthread is dynamically generated and derived from the main application thread.

> **中文翻译：** 解耦向量 Runahead 克服了上一节列出的缺点，如下所示。当核心发现它正在执行具有相关负载的循环时，基于可用于预测未来循环迭代的步幅负载，将在与当前执行的主线程相同的核心上激活专门的向量运行子线程。该子线程是动态生成的，以预取将来的许多内存访问，但不会影响主线程的语义。向量运行子线程在同一内核上与主线程一起运行，很像线程在同步多线程（SMT）[91]中共同执行的方式，不同之处在于子线程是微架构生成的、瞬态的（预取到缓存中而不是实现真正的计算）、推测性的、重新排序以实现极高的内存级别并行性，并且明显更简单，即子线程按顺序执行。向量运行子线程也与同步从属微线程[20]密切相关，后者也旨在提高主线程的性能。从属微线程是用具有专门的机器特定指令的微代码编写的，而向量运行子线程是从主应用程序线程动态生成和派生的。

To achieve high memory-level parallelism from this in-order vector-runahead subthread, even while following chains of dependent loads that stall the subthread, we use single-instruction multiple-thread (SIMT) data-level parallelism [54], to execute large numbers of each instruction from the front-end, each representing a different loop iteration, simultaneously, thereby prefetching far into the future. Since this happens continuously, and overlaps with the execution of the main thread, most of the main out-of-order thread’s memory accesses hit in the L1 by the time it reaches them — thus even for very large processors with massive windows, significant speedups can be achieved.

> **中文翻译：** 为了从这个有序向量运行子线程中实现高内存级并行性，即使在遵循导致子线程停顿的依赖负载链时，我们也使用单指令多线程（SIMT）数据级并行性[54]，从前端执行大量的每条指令，每条指令同时代表不同的循环迭代，从而预取到遥远的未来。由于这种情况连续发生，并且与主线程的执行重叠，因此大多数主乱序线程的内存访问在到达 L1 时都会命中 — 因此，即使对于具有大量窗口的非常大的处理器，也可以实现显着的加速。

Figure 3 provides a schematic of a processor’s microarchitecture enhanced to support DVR. We explain the various components in the following sections.

> **中文翻译：** 图 3 提供了增强支持 DVR 的处理器微架构的示意图。我们将在以下部分中解释各个组件。

## 4.1 Discovery Mode / 4.1 发现模式

To discover an induction-variable load that multiple future copies of a loop can be spawned from, as in the original Vector Runahead proposal [67], we use a stride detector to identify a striding load and its stride, i.e., a load that follows a regular address sequence. Once we have this information, we enter _Discovery Mode_ to perform a series of new analyses. The purpose of Discovery Mode is to (i) check whether the striding load is the most suitable candidate for DVR, by being the innermost striding load, (ii) derive the loop bounds, to determine how many speculative vector prefetches to generate, and (iii) discover whether there are any dependent loads based on the striding load that can be suitably prefetched by the vector-runahead subthread. Discovery Mode follows the main thread’s execution through one iteration of the loop, until it reaches the striding load again, at which point it exits Discovery Mode.

> **中文翻译：** 为了发现可以从中产生循环的多个未来副本的归纳变量负载，如原始 Vector Runahead 提案 [67] 中所示，我们使用步幅检测器来识别步幅负载及其跨步，即遵循常规地址序列的负载。一旦我们获得了这些信息，我们就会进入发现模式来执行一系列新的分析。发现模式的目的是 (i) 通过作为最内层的步幅负载来检查步幅负载是否是最适合 DVR 的候选者，(ii) 导出循环边界，以确定要生成多少个推测向量预取，以及 (iii) 发现是否存在基于步幅负载的任何相关负载，这些负载可以由向量运行超前子线程适当地预取。发现模式通过一次循环迭代跟随主线程的执行，直到再次达到步幅负载，此时退出发现模式。

_4.1.1 Innermost Striding-Load Detection._ Once an initial striding load is detected and Discovery Mode is engaged, we follow the main thread’s execution to detect other striding loads that could be better candidates for initiating vector runahead. In particular, we may discover a striding load that is part of a more inner loop, and thus whose future iterations will be more timely if we prefetch them during vector-runahead mode. Striding load detection is done using the _Reference Prediction Table (RPT)_ [22, 63], which keeps track of all striding loads and their strides. We keep a register initialized to zero with one bit per RPT entry. Stride loads set their bit to 1. If already set, then we have seen the same stride-load PC twice during Discovery Mode before seeing the current target stride again. This means the new stride is more inner, so we switch to performing Discovery Mode on it instead, resetting this register, the VTT and FLR (Section 4.1.2). We can vectorize multiple strides in the same loop (e.g., caused by loop unrolling), and this process simply chooses one to be the trigger, preferring innermost strides.

> **中文翻译：** 4.1.1 最里面的步幅负载检测。一旦检测到初始步幅负载并启用发现模式，我们就会跟踪主线程的执行来检测其他步幅负载，这些负载可能是启动向量前瞻执行的更好候选者。特别是，我们可能会发现一个步幅负载，它是更内部循环的一部分，因此如果我们在向量运行模式下预取它们，其未来的迭代将更加及时。跨步负荷检测是使用参考预测表 (RPT) [22, 63] 完成的，它跟踪所有跨步负荷及其步幅。我们将寄存器初始化为零，每个 RPT 条目一位。步幅负载将其位设置为 1。如果已经设置，那么我们会在发现模式期间两次看到相同的步幅负载 PC，然后再次看到当前目标步幅。这意味着新的步幅更加内在，因此我们改为对其执行发现模式，重置该寄存器、VTT 和 FLR（第 4.1.2 节）。我们可以在同一循环中对多个步幅进行向量化（例如，由循环展开引起），并且此过程只需选择一个作为触发器，优先选择最里面的步幅。

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0005-47.png)

![](Decoupled_Vector_Runahead_assets/figure-03-dvr-pipeline.png)

**Figure 3: DVR processor pipeline.** _The stride detector obtains information about loads from the dispatch and execute stages of the pipeline. Once a stride is detected, DVR enters Discovery Mode, which uses the Taint Tracker and Loop-Bound Detector to discover information for the subsequent runahead. The Nested Discovery Mode logic will be used if Discovery Mode finds too few elements of the loop to vectorize. Once Discovery Mode is complete, the vector program counter (𝑃𝐶𝑣) will be populated with the PC of the striding load, the VRAT will be populated with the striding load addresses and a copy of the main thread’s scalar registers, and the decoupled vector-runahead subthread will initiate. The Reconvergence Stack will engage upon divergence in control-flow between the vector lanes._

> **中文翻译：** 图 3：DVR 处理器管道。步幅检测器从管道的调度和执行阶段获取有关负载的信息。一旦检测到跨步，DVR 就会进入发现模式，该模式使用污点跟踪器和循环绑定检测器来发现后续运行的信息。如果发现模式发现循环中需要向量化的元素太少，则将使用嵌套发现模式逻辑。发现模式完成后，向量程序计数器 (𝑃𝐶𝑣) 将填充跨步加载的 PC，VRAT 将填充跨步加载地址和主线程标量寄存器的副本，并且将启动解耦向量运行子线程。再收敛堆栈将在向量通道之间的控制流出现分歧时进行处理。

_4.1.2 Dependent-Load Checking._ For DVR to be worth triggering, it must bring useful data into the cache beyond that of a simple stride prefetcher [22], which we always assume such a system will have (and always leave enabled). This means there must be further loads dependent on the value identified via the stride detector for it to be worth initiating vector runahead. We use a small _Vector Taint Tracker (VTT)_ , featuring a single bit per architectural integer register, to identify instructions that will later be vectorized. At the start of Discovery Mode, the VTT is initialized to all zeroes, except for the destination architecture register of the initiating striding load, which is set to one. This taint then propagates via instructions whose source register is tainted, transitively. If an instruction writes to a register whose taint bit is set but whose source registers are not, the taint bit of the target is reset. Whenever an input to a load is tainted in the VTT, the _Final-Load Register (FLR)_ (initialized to zero at the start of Discovery Mode) is updated with the load PC. The FLR is a register that holds a single load PC, and its purpose is to identify the last load in the dependence chain originating from the striding load. The idea is then to vectorize all (tainted) instructions in the dependence chain starting from the striding load up until this last dependent load in the FLR. A non-zero FLR at the end of Discovery Mode indicates a load-dependence chain.

> **中文翻译：** 4.1.2 相关负载检查。为了使 DVR 值得触发，它必须将有用的数据带入缓存，而不仅仅是简单的跨步预取器 [22]，我们总是假设这样的系统将具有这种功能（并且始终保持启用状态）。这意味着必须有更多的负载取决于通过步幅检测器识别的值，才值得启动向量前瞻执行。我们使用一个小型向量污点跟踪器（VTT），每个架构整数寄存器有一个位，来识别稍后将被向量化的指令。在发现模式开始时，VTT 被初始化为全零，除了初始跨步加载的目标架构寄存器被设置为 1。然后，该污染通过源寄存器被污染的指令进行传播。如果指令写入污染位已设置但源寄存器未设置的寄存器，则目标的污染位将被重置。每当负载的输入在 VTT 中受到污染时，最终负载寄存器 (FLR)（在发现模式开始时初始化为零）就会随负载 PC 一起更新。 FLR 是一个保存单个负载 PC 的寄存器，其目的是识别源自步幅负载的依赖链中的最后一个负载。然后，我们的想法是对依赖链中的所有（受污染的）指令进行向量化，从跨步加载开始，直到 FLR 中的最后一个依赖加载。发现模式末尾的非零 FLR 表示负载依赖链。

_4.1.3 Loop-Bound Inference._ The next step is to determine how many iterations are left for the inner loop to execute. This enables determining how many speculative vector prefetches to initiate during vector runahead. Doing so avoids generating wasteful and/or counterproductive loads that are out-of-bounds of the loop we expect to execute. During Discovery Mode, we look for the first branch with a backward edge, indicating a loop. The compare instruction that provides the source operand to this backward branch is used to determine the loop bound. In particular, we have both a Last-Compare Register (LCR) and a Seen-Branch Bit (SBB), which are zeroed whenever we update the Final-Load Register. If we see a compare instruction and the SBB is zero, we set the LCR with the compare’s source and destination architectural register IDs. If we see a branch whose source matches the LCR destination and whose branch-taken destination is less than or equal to the striding load’s PC,<sup>1</sup> then we set the SBB, to indicate that we should not alter the LCR unless we see a new final load.

> **中文翻译：** 4.1.3 循环限制推理。下一步是确定内部循环还需要执行多少次迭代。这使得能够确定在向量前瞻执行期间要启动多少推测向量预取。这样做可以避免产生浪费和/或适得其反的负载，这些负载超出了我们期望执行的循环范围。在发现模式期间，我们寻找第一个具有后向边的分支，表明存在循环。向该向后分支提供源操作数的比较指令用于确定循环界限。特别是，我们有一个最后比较寄存器（LCR）和一个已见分支位（SBB），每当我们更新最终加载寄存器时，它们都会被清零。如果我们看到比较指令并且 SBB 为零，则我们使用比较的源和目标架构寄存器 ID 设置 LCR。如果我们看到一个分支，其源与 LCR 目标匹配，并且其分支采用的目标小于或等于步幅负载的 PC，[1]，那么我们设置 SBB，以指示我们不应更改 LCR，除非我们看到新的最终负载。

We also take two checkpoints of the architectural register file: one upon entering Discovery Mode, and one upon leaving it. We then check the register mappings of the inputs to the identified compare instruction. If one stays constant for the whole Discovery Mode, and the other changes, we use (i) the constant value as the loop bound, and (ii) the difference in the changing value as the loop increment. This provides enough information to determine the remaining iterations of the loop. If we fail to produce a match, then we run for 128 elements, the limit for any invocation of DVR.<sup>2</sup>

> **中文翻译：** 我们还对架构寄存器文件进行两个检查点：一个在进入发现模式时检查，一个在离开发现模式时检查。然后，我们检查输入到所识别的比较指令的寄存器映射。如果一个在整个发现模式中保持不变，而另一个发生变化，我们使用 (i) 恒定值作为循环界限，以及 (ii) 变化值的差作为循环增量。这提供了足够的信息来确定循环的剩余迭代。如果我们无法产生匹配，那么我们将运行 128 个元素，这是任何 DVR 调用的限制。[2]

## 4.2 Vector-Runahead Subthread Operation / 4.2 向量前瞻子线程的运行

Once Discovery Mode has identified a striding load, its stride, its dependence chain and the remaining iterations of the inner loop, the vector-runahead subthread is spawned once the main thread reaches the candidate striding load again. The subthread starts from the striding load and ends at the PC stored in the FLR, with the goal of speculatively prefetching a large number (up to 128 in our setup) of vectorized copies. In particular, the _Vectorizer_ replaces the striding load by vectorized copies generated using its stride. Any instruction in the future instruction stream that depends on the striding load also gets vectorized.

> **中文翻译：** 一旦发现模式识别出步幅负载、其跨步、其依赖链以及内部循环的剩余迭代，一旦主线程再次达到候选步幅负载，就会生成向量运行超前子线程。子线程从跨步加载开始，到存储在 FLR 中的 PC 结束，其目标是推测性地预取大量（在我们的设置中最多 128 个）向量化副本。特别是，向量化器通过使用其步幅生成的向量化副本来替换步幅负载。未来指令流中依赖于步幅负载的任何指令也会被向量化。

> 1If we see other branches between the FLR and the LCR, we ignore the FLR and allow each runahead lane to continue onto the next stride PC, to allow it to fully explore any divergent paths that may manifest. The FLR is still used in Discovery Mode to help identify the loop, which must always encapsulate both the stride load and the FLR load.

> **中文翻译：** 1如果我们看到 FLR 和 LCR 之间的其他分支，我们会忽略 FLR 并允许每个超前通道继续到下一个跨步 PC，以允许它充分探索可能出现的任何不同路径。 FLR 仍用于发现模式，以帮助识别循环，循环必须始终封装步幅负载和 FLR 负载。

> 2Runahead is transient execution and does not need to be correct, and so the goal for using more complex heuristics is only to reduce under/overfetching.

> **中文翻译：** 2Runahead 是瞬时执行，不需要正确，因此使用更复杂的启发式方法的目标只是减少获取不足/过度获取。

|Vreg||||Preg|(s)||||
|---|---|---|---|---|---|---|---|---|
|R1|S45|S45|S45|S45|S45|S45|S45|S45|
|R2|V34|V35|V36|V37|V38|V39|V68|V69|

**中文表格：**

|电压||||预浸料|(s)||||
|---|---|---|---|---|---|---|---|---|
|R1|S45|S45|S45|S45|S45|S45|S45|S45|
|R2|V34|V35|V36|V37|V38|V39|V68|V69|

**Figure 4: An example VRAT allocation considering 8 physical registers (one per vector lane) for brevity rather than 16 as in our setup.** _Architectural register R1 points to the same scalar physical register (S45) for all lanes. Architectural register R2 has been vectorized to 8 different vector physical registers, because either one of its sources was tainted, or control-flow divergence occurred._

> **中文翻译：** 图 4：为了简洁起见，VRAT 分配示例考虑了 8 个物理寄存器（每个向量通道一个），而不是我们设置中的 16 个。架构寄存器 R1 指向所有通道的相同标量物理寄存器 (S45)。架构寄存器 R2 已被向量化为 8 个不同的向量物理寄存器，因为其源之一被污染，或者发生了控制流分歧。

The subthread uses the same fetch, decode and execute units as the main thread. Subthread instructions are generated from the front-end buffer, which decouples the fetch stage from the rest of the pipeline by holding decoded micro-ops (eight in our setup). While subthread instructions use the same execution units, they use a different _Vector Issue Register (VIR)_ — rather than an out-of-order instruction queue, as it is in-order — to handle execution of the vector instruction copies. An instruction in the vector-runahead subthread’s issue register is issued whenever there is no instruction ready from the main thread for the same execution port.

> **中文翻译：** 子线程使用与主线程相同的获取、解码和执行单元。子线程指令是从前端缓冲区生成的，前端缓冲区通过保存解码的微操作（在我们的设置中为八个）将提取阶段与管道的其余部分解耦。虽然子线程指令使用相同的执行单元，但它们使用不同的向量发出寄存器（VIR）——而不是乱序指令队列，因为它是有序的——来处理向量指令副本的执行。每当主线程没有为同一执行端口准备好指令时，就会发出向量运行子线程的发出寄存器中的指令。

_4.2.1 Vector Register Allocation Table._ The vector register allocation table (VRAT) stores the subthread’s current mapping from architecture scalar registers to physical registers. Even though the subthread is in-order, we still need to rename its architectural registers because it shares the physical scalar and vector register files with the main thread. The VRAT stores multiple physical (scalar or vector) registers for each scalar architectural integer register. As illustrated in Figure 4, a scalar architectural register can be renamed to (i) the same scalar physical register in all vector lanes, in the case where the architectural register is not vectorized and there is no control-flow divergence across lanes, or (ii) multiple vector physical registers, where the architectural register has been vectorized or there is control-flow divergence.

> **中文翻译：** 4.2.1 向量寄存器分配表。向量寄存器分配表（VRAT）存储子线程当前从架构标量寄存器到物理寄存器的映射。即使子线程是有序的，我们仍然需要重命名其架构寄存器，因为它与主线程共享物理标量和向量寄存器文件。 VRAT 为每个标量架构整数寄存器存储多个物理（标量或向量）寄存器。如图 4 所示，标量架构寄存器可以重命名为 (i) 所有向量通道中的相同标量物理寄存器（在架构寄存器未向量化且跨通道不存在控制流分歧的情况下），或 (ii) 多个向量物理寄存器（在架构寄存器已向量化或存在控制流分歧的情况下）。

To initialize the VRAT, all architectural registers from the main thread are allocated a fresh physical scalar register to decouple the subthread from its main thread. When the striding load is issued to the VIR, we allocate 16 vector (e.g., AVX-512) physical registers to map the load’s target architectural register to. Unlike in an outof-order processor, physical registers are not remapped with every new instruction, since the renaming is not trying to remove WAW nor WAR dependencies, i.e., the subthread executes in program order. Instead, we allocate new physical registers in only two cases. First, when one of the source registers has been vectorized (because it depends on the striding load), but the destination register has not yet been vectorized — at which point we must select 16 free vector physical registers to map to. Second, if the destination register is a vectorized register, but is about to be overwritten by a scalar instruction — this may occur as a result of a WAW dependence in the original program code — it is renamed to a scalar physical register from the free list. When only a subset of lanes are being executed, due to branch divergence, only some registers are renamed, as described in Section 4.2.3.

> **中文翻译：** 为了初始化 VRAT，主线程的所有架构寄存器都被分配一个新的物理标量寄存器，以将子线程与其主线程解耦。当跨步加载被发送到 VIR 时，我们分配 16 个向量（例如 AVX-512）物理寄存器来映射加载的目标架构寄存器。与乱序处理器不同，物理寄存器不会用每条新指令重新映射，因为重命名不会尝试删除 WAW 或 WAR 依赖性，即子线程按程序顺序执行。相反，我们仅在两种情况下分配新的物理寄存器。首先，当其中一个源寄存器已被向量化（因为它取决于步幅负载），但目标寄存器尚未向量化时，此时我们必须选择 16 个空闲向量物理寄存器进行映射。其次，如果目标寄存器是向量化寄存器，但即将被标量指令覆盖（这可能是由于原始程序代码中的 WAW 依赖性而发生），则它将被重命名为空闲列表中的标量物理寄存器。当仅执行通道的子集时，由于分支分歧，仅重命名一些寄存器，如第 4.2.3 节所述。

Physical registers are returned to the free list once they are overwritten. Overwritten registers are freed immediately, provided they are not used as a source register for the instruction to be

> **中文翻译：** 物理寄存器一旦被覆盖就会返回到空闲列表。被覆盖的寄存器会立即释放，前提是它们不被用作要执行的指令的源寄存器。

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0006-08.png)

**Figure 5: The Vector Issue Register showing 4 AVX-512 vector instructions (instead of 16 as in our setup for brevity).** _Finegrained masking has turned some scalar-equivalent lanes in AVX-512 instructions 0 and 2 into no-ops. The first AVX-512 instruction has been issued and executed, and the last three have neither been issued nor executed. Source register src1 is scalar register S3 and is shared among all lanes (none of which have diverged), which may be for example the base address of an array, whereas source register src2 has been vectorized (for example the index into the array). The destination registers are also vectorized, to the same location as src2 as they were the same architectural scalar register._

> **中文翻译：** 图 5：向量发出寄存器显示 4 条 AVX-512 向量指令（而不是我们设置中为简洁起见而设置的 16 条）。细粒度屏蔽已将 AVX-512 指令 0 和 2 中的一些标量等效通道变为无操作。第一条 AVX-512 指令已发出并执行，后 3 条指令既没有发出也没有执行。源寄存器 src1 是标量寄存器 S3，并且在所有通道之间共享（没有一个通道发生分歧），例如，它可以是数组的基地址，而源寄存器 src2 已被向量化（例如数组的索引）。目标寄存器也被向量化，位于与 src2 相同的位置，因为它们是相同的架构标量寄存器。

issued — otherwise they are freed after execute, and tracked in the Vector Issue Register via the ‘dead-source’ bits (since it occurs after the overwriting occurs within the VRAT), as discussed in the next section.

> **中文翻译：** 发出 — 否则它们在执行后被释放，并通过“死源”位在向量发出寄存器中进行跟踪（因为它发生在 VRAT 内发生覆盖之后），如下一节所述。

_4.2.2 Vector Issue Register._ To achieve a significantly higher degree of memory-level parallelism than a single vector register (8 64-bit loads, as for AVX-512), we overlap the execution of multiple vector copies of the same instruction, with the target of achieving 16 AVX-512 vectors (or 16 × 8 = 128 scalar-equivalent loops) in-flight simultaneously. Instead of using a scalar issue queue, we use a single Vector Issue Register (VIR), responsible for the issuing of each vector copy of the scalar instruction (Figure 5).

> **中文翻译：** 4.2.2 向量发布寄存器。为了实现比单个向量寄存器（8 个 64 位负载，如 AVX-512）更高程度的内存级并行性，我们重叠执行同一指令的多个向量副本，目标是同时实现 16 个 AVX-512 向量（或 16 × 8 = 128 个标量等效循环）。我们不使用标量发布队列，而是使用单个向量发布寄存器 (VIR)，负责发布标量指令的每个向量副本（图 5）。

If all inputs to the instruction are scalars, then just a single scalar instruction is issued. If the instruction is marked as a striding load, we use the stride detector to fill in all 128 values, and issue these as 16 vectorized AVX-512 loads. If the instruction depends on at least one vectorized input, we likewise issue 16 vectorized copies of the instruction in sequence to the execution units. Vectorized instruction copies are issued to the execution units whenever a suitable unit is free (not being used by the main thread). Within one AVX-512 instruction, we have 8 mask bits, to indicate lanes where one of the sources has been marked invalid, either through a fault, through use of floating-point registers, or through control-flow divergence. Some lanes may start as masked out, if Discovery Mode’s loop-bound inference predicts that there will be less than 128 scalar-equivalent loops it can fetch. Once all instruction copies have issued and executed, if the ‘dead-source’ bit is set on any of the sources, the physical registers are freed. Then, we fetch the next instruction, and repeat.

> **中文翻译：** 如果指令的所有输入都是标量，则仅发出单个标量指令。如果指令被标记为跨步加载，我们使用步幅检测器填充所有 128 个值，并将它们作为 16 个向量化 AVX-512 加载发出。如果指令依赖于至少一个向量化输入，我们同样会按顺序向执行单元发出该指令的 16 个向量化副本。只要有合适的单元空闲（未被主线程使用），向量化指令副本就会发送到执行单元。在一条 AVX-512 指令中，我们有 8 个掩码位，用于指示其中一个源已被标记为无效的通道，无论是通过故障、通过使用浮点寄存器还是通过控制流发散。如果发现模式的循环限制推理预测它可以获取的标量等效循环将少于 128 个，则某些通道可能会在开始时被屏蔽。一旦所有指令副本都已发出并执行，如果在任何源上设置了“死源”位，则物理寄存器将被释放。然后，我们获取下一条指令，然后重复。

Vectorized load instructions are treated like vector gather operations [87]: they are split into scalar loads in the LSQ and sent to the

> **中文翻译：** 向量化加载指令被视为向量收集操作[87]：它们在 LSQ 中被拆分为标量加载并发送到

|PC(48 bits)|Mask(128 bits)|
|---|---|
|0x1234|111111100000|
|0x12a0|000000011111|

**中文表格：**

|电脑（48位）|掩码(128位)|
|---|---|
|0x1234|111111100000|
|0x12a0|000000011111|

**Figure 6: An example reconvergence stack.** _The top of the stack stores the current PC and mask. Once the reconvergence point is reached, the stack head is popped and execution proceeds with the next PC and mask._

> **中文翻译：** 图 6：重新收敛堆栈示例。栈顶存储当前的PC和掩码。一旦到达重新收敛点，堆栈头就会弹出，并继续执行下一个 PC 和掩码。

cache hierarchy individually. The memory system handles them concurrently with other regular scalar loads, allocating a different MSHR.

> **中文翻译：** 单独缓存层次结构。内存系统与其他常规标量加载同时处理它们，分配不同的 MSHR。

_4.2.3 Branch Reconvergence._ Dependent loads may be conditional, i.e., they appear down some control-flow paths and not others inside the inner loop. We allow each scalar-equivalent lane to diverge from the others. We therefore use a GPU-like reconvergence stack [54]. The results of the branches in all active lanes are compared against each other. If the next PC for any lane diverges from the others, we split the lanes based on their new destination, generate masks based on common groups, and place the masks and target PCs onto a reconvergence stack (Figure 6). We follow the first lane all the way to the reconvergence point, which we set to the vector-runahead termination point (Section 4.2.4), to avoid special tracking. Once we reach the termination point for a set of matching lanes, we pop the head off the reconvergence stack, reset the masks, and proceed from the next PC in the stack.

> **中文翻译：** 4.2.3 分支重新收敛。相关负载可能是有条件的，即它们出现在某些控制流路径上，而不是内循环内的其他路径上。我们允许每个标量等效通道与其他通道分开。因此，我们使用类似 GPU 的再收敛堆栈 [54]。所有活动通道中的分支结果都会相互比较。如果任何通道的下一个 PC 与其他通道不同，我们会根据新的目的地分割通道，根据公共组生成掩码，并将掩码和目标 PC 放置到重新收敛堆栈上（图 6）。我们沿着第一条通道一直到达重新收敛点，我们将其设置为向量前瞻执行终止点（第 4.2.4 节），以避免特殊跟踪。一旦我们到达一组匹配通道的终止点，我们就会将头从再收敛堆栈中弹出，重置掩码，然后从堆栈中的下一个 PC 继续。

Each lane is simultaneously mapped in the VRAT. If we have divergence in scalar renaming (because we use different scalars), and this divergence occurs neatly across AVX-512 instruction boundaries, then we overwrite each scalar according to which of the 16 AVX-512 instructions use it. If we have divergence in scalar renaming within an AVX-512 instruction, we convert the destination to an AVX-512 physical register, and copy the scalar values being replaced.

> **中文翻译：** 每个通道同时映射到 VRAT 中。如果我们在标量重命名中存在分歧（因为我们使用不同的标量），并且这种分歧恰好发生在 AVX-512 指令边界上，那么我们会根据 16 个 AVX-512 指令中的哪一个使用它来覆盖每个标量。如果 AVX-512 指令中的标量重命名存在分歧，我们会将目标转换为 AVX-512 物理寄存器，并复制要替换的标量值。

_4.2.4 Termination._ The vector-runahead subthread terminates when the lanes reach the final indirect load in the sequence (identified by the FLR), or the next iteration of the stride PC in the case of divergence, with a 200-instruction timeout (in case we leave the loop entirely in a way not picked up by the loop bound detector, e.g., via a break).

> **中文翻译：** 4.2.4 终止。当通道到达序列中的最终间接负载（由 FLR 识别）时，向量运行子线程终止，或者在发散的情况下达到步幅 PC 的下一次迭代，并具有 200 个指令超时（如果我们以循环绑定检测器未拾取的方式完全离开循环，例如通过中断）。

The main thread executes concurrently with the vector-runahead subthread. Once the subthread has terminated, the main thread again becomes eligible for entering Discovery Mode the next time it executes a striding load, and thus for re-initiating DVR. The main thread will have made significant progress by this point, and most of its cache accesses will become L1 hits, provided the DVR subthread was accurate and timely.

> **中文翻译：** 主线程与向量运行子线程同时执行。一旦子线程终止，主线程下次执行跨步加载时将再次有资格进入发现模式，从而重新启动 DVR。此时主线程将取得重大进展，并且如果 DVR 子线程准确且及时，其大部分缓存访问将成为 L1 命中。

## 4.3 Nested Vector Runahead / 4.3 嵌套向量前瞻执行

Loop-bound inference (Section 4.1.3) provides an accurate count of how many iterations each loop will execute, and thus how many scalar-equivalent lanes DVR can fill with useful prefetches. This may well be significantly lower than the 128-element maximum we can achieve, if each inner loop is relatively short, hurting the total memory-level parallelism, and thus limiting the benefits of the latency overlapping achieved by DVR.

> **中文翻译：** 循环绑定推理（第 4.1.3 节）提供了每个循环将执行多少次迭代的准确计数，以及 DVR 可​​以用有用的预取填充多少标量等效通道。如果每个内部循环相对较短，这很可能大大低于我们可以实现的 128 个元素的最大值，从而损害总内存级并行性，从而限制 DVR 实现的延迟重叠的好处。

The goal of the Nested Vector Runahead is to find iterations from multiple _invocations_ of a loop when the loop bound detector does not find enough upcoming iterations of the innermost striding load (Section 4.1.1). The Nested Vector Runahead benefits benchmarks with patterns shown in Algorithm 1. If the for loop at line 7 has a small number of iterations, vectorizing the chain starting from the _inner_ striding load at line 8 cannot generate high MLP. Therefore, it is critical to prefetch indirect chains from many invocations of the for loop. Nested Vector Runahead works in two steps. First, it performs a _Nested Discovery Mode (NDM)_ to vectorize the chain of instructions from the _outer_ striding load to the _inner_ striding load, and discover loop bounds and data inputs to multiple invocations of the inner loop. Second, upon reaching the inner striding loop, it expands vectorization further to cover the inner loop as well.

> **中文翻译：** 嵌套向量 Runahead 的目标是当循环边界检测器找不到足够的最内步幅负载的即将到来的迭代时，从循环的多次调用中查找迭代（第 4.1.1 节）。嵌套向量 Runahead 有利于算法 1 中所示模式的基准测试。如果第 7 行的 for 循环迭代次数较少，则从第 8 行的内部步幅负载开始对链进行向量化无法生成高 MLP。因此，从 for 循环的多次调用中预取间接链至关重要。嵌套向量 Runahead 分两步工作。首先，它执行嵌套发现模式 (NDM)，将指令链从外部跨步加载向量化到内部跨步加载，并发现循环边界和内部循环多次调用的数据输入。其次，在到达内部跨步循环时，它进一步扩展向量化以覆盖内部循环。

_4.3.1 Nested Discovery Mode._ The goal of the NDM is to find the starting striding addresses and loop bounds for many different invocations of the inner loop at the same time. During a discovery mode (Section 4.1), the loop-bound detector may find fewer than 64 upcoming iterations of a loop. In this case, once the vector-runahead subthread is spawned, instead of performing vector runahead immediately, we alter the direction of the branch with the backward edge (see Section 4.1.3) and begin NDM on the in-order subthread by setting PCv to the instruction following the branch (not-taken path instruction). The subthread runs concurrently with the main thread. We still save both the source registers in the LCR. The constant loop increment and address of the striding load are saved in two new registers called _Increment Register (IR)_ and _Inner Load Register (ILR)_ , respectively.

> **中文翻译：** 4.3.1 嵌套发现模式。 NDM 的目标是同时找到内循环的许多不同调用的起始跨步地址和循环边界。在发现模式（第 4.1 节）期间，循环限制检测器可能会发现少于 64 个即将到来的循环迭代。在这种情况下，一旦产生向量运行子线程，我们不是立即执行向量运行，而是用后向边改变分支的方向（参见第 4.1.3 节），并通过将 PCv 设置为分支后面的指令（未采取的路径指令）来在有序子线程上开始 NDM。子线程与主线程同时运行。我们仍然将两个源寄存器保存在 LCR 中。恒定循环增量和跨步加载的地址分别保存在两个新寄存器中，分别称为增量寄存器（IR）和内部加载寄存器（ILR）。

The NDM subthread begins executing scalar operations, but skips all the upcoming iterations of the inner loop due to the altered branch direction, and executes instructions outside the inner loop. When it finds an outer striding load with an address smaller than the address in the ILR (e.g., line 4 versus line 8 in Algorithm 1), it performs its first vectorization step: it vectorizes the striding load (by a factor of 16, to attempt to find at least 128 viable inner loop iterations) and marks the load’s destination in the taint vector.

> **中文翻译：** NDM 子线程开始执行标量操作，但由于分支方向更改而跳过内部循环的所有即将进行的迭代，并执行内部循环外部的指令。当它发现一个外部步幅负载的地址小于 ILR 中的地址时（例如，算法 1 中的第 4 行与第 8 行），它会执行第一个向量化步骤：它对步幅负载进行向量化（乘以 16 倍，以尝试找到至少 128 个可行的内部循环迭代），并在污点向量中标记负载的目的地。

The process of vectorization continues for the dependents of each outer striding load — until it reaches the first iteration of each inner striding load. In Algorithm 1, the outer striding load at line 4 has dependents at both line 5 and line 6.

> **中文翻译：** 对于每个外部步幅负载的依赖项，向量化过程继续进行，直到到达每个内部步幅负载的第一次迭代。在算法 1 中，第 4 行的外部步幅负载在第 5 行和第 6 行都有相关性。

When it it reaches the inner striding load (at line 8), it reads the values of the vectorized copies of the source registers in the LCR, and uses these and the value in IR to calculate the number of invocations of the inner loops for each of our vectorized outer loops. If no outer striding load with an address lower than the inner striding load appears within 200 instructions after entry to the NDM, the subthread re-calculates the loop bound based on the values in LCR and IR, and vectorizes the inner striding load by the loop bound. That is, the subthread resorts back to the number of iterations calculated by the loop bound detector during the initial discovery mode.

> **中文翻译：** 当它达到内部跨越负载（第 8 行）时，它读取 LCR 中源寄存器的向量化副本的值，并使用这些值和 IR 中的值来计算每个向量化外循环的内部循环调用次数。如果进入 NDM 后 200 条指令内没有出现地址低于内部步幅负载的外部步幅负载，则子线程根据 LCR 和 IR 中的值重新计算循环边界，并通过循环边界向量化内部步幅负载。也就是说，子线程诉诸于初始发现模式期间由循环界限检测器计算的迭代次数。

_4.3.2 Further Vectorization._ Based on the loop bounds detected, the NDM subthread then collects as many striding inner addresses as possible with a maximum limit of 128. Addresses beyond the first

> **中文翻译：** 4.3.2 进一步向量化。根据检测到的循环边界，NDM 子线程会收集尽可能多的跨步内部地址，最大限制为 128。超出第一个的地址

128 are discarded. The NDM subthread then performs vectorization from the inner striding load, by populating its vector registers with these 128 targets, with all other registers set based on which of the 16 outer-loop lanes it was spawned from (scalar for currently untainted registers, and vector for registers tainted in NDM). It taints the destination of the inner striding load and enters DVR with each lane, starting and terminating as specified in Section 4.2.

> **中文翻译：** 128 被丢弃。然后，NDM 子线程通过使用这 128 个目标填充其向量寄存器，并根据其所生成的 16 个外循环通道中的哪一个来设置所有其他寄存器（当前未受污染的寄存器的标量，以及 NDM 中受污染的寄存器的向量），从内部跨步加载执行向量化。它污染内部步幅负载的目的地，并进入每个通道的 DVR，按照第 4.2 节中的规定开始和终止。

**Table 1: Baseline configuration for the OoO core.**

> **中文翻译：** 表 1：OoO 核心的基准配置。

|Core|4.0 GHz, out-of-order|
|---|---|
|ROB size|350|
|Queue sizes|issue (128), load (128), store (72)|
|Processor width<br>Pipeline depth<br>Branchpredictor|5-wide fetch/dispatch/rename/commit<br>15 front-end stages<br>8 KB TAGE-SC-L|
|Functional units|4 int add (1 cycle), 1 int mult (3 cycles),<br>1 int div (18 cycles), 1 fp add (3 cycles),<br>1 fpmult(5 cycles), 1 fpdiv(6 cycles)|
|Vector units|3 ALU, 2 shift, 2 add, 2 mul, 2 shufe|
|Register fle|256 int (64 bit)<br>256 fp (128 bit)<br>128 vector(512 bit)|
|L1 I-cache|32 KB, assoc 4, 2-cycle access|
|L1 D-cache|32 KB, assoc 8, 4-cycle access,<br>24 MSHRs, stride prefetcher (16 streams)|
|Private L2 cache|256 KB, assoc 8, 8-cycle access|
|Shared L3 cache|8 MB, assoc 16, 30-cycle access|
|Memory|50 ns min. latency, 51.2 GB/s bandwidth,<br>request-based contention model|

**中文表格：**

|核|4.0 GHz，乱序|
|---|---|
|罗布尺寸|350|
|队列大小|发出 (128)、加载 (128)、存储 (72)|
|处理器宽度<br管道深度<br分支预测器|5 宽获取/调度/重命名/提交<br15 个前端阶段<br8 KB TAGE-SC-L|
|功能单位|4 个 int add（1 个周期）、1 个 int mult（3 个周期）、<br1 int div（18 个周期）、1 个 fp add（3 个周期）、<br1 fpmult（5 个周期）、1 个 fpdiv（6 个周期）|
|向量单位|3 个 ALU、2 个移位、2 个加法、2 个乘法、2 个舒夫|
|寄存器文件|256 int（64 位）<br256 fp（128 位）<br128 向量（512 位）|
|L1 I 缓存|32 KB，关联 4，2 周期访问|
|L1 D 缓存|32 KB，关联 8，4 周期访问，<br24 MSHR，跨步预取器（16 个流）|
|私有二级缓存|256 KB，关联 8，8 周期访问|
|共享三级缓存|8 MB，关联 16，30 周期访问|
|内存|最小 50 纳秒延迟、51.2 GB/s 带宽、<br基于请求的争用模型|

## 4.4 Hardware Overhead / 4.4 硬件开销

The hardware structures to support DVR incur only 1139 bytes overhead. The 32-entry stride detector requires 460 bytes: each entry incurs 48 bits for the load PC, 48 bits for the previous memory address, 16 bits for the stride distance, 2 bits for the saturating counter, and 1 bit for innermost detection. The VRAT is a 16-entry table (288 bytes): each entry features 16 register identifiers each requiring 9 bits (to select one of the 128 vector physical registers and 256 integer physical registers). The VIR incurs 86 bytes: 128 bits for the mask, 16 bits issued, 16 bits executed, 64 bits uop and imm, 9×16 bits for the destination, 10×16 bits for src1, 10×16 bits for src2. The front-end buffer incurs 64 bytes for 8 micro-ops. The 8-entry reconvergence stack requires 176 bytes: 6 bytes for the PC and 128-bit mask for each PC. The FLR and LCR require only 6 bytes and 2 bytes, respectively; the SBB requires only 1 bit. The loop-bound detector saves two checkpoints (2×16×8 bits for the register ID mappings) and two registers for the compare and branch instructions, totalling 48 bytes. The taint-tracker needs 16 bits. For NDM, the IR and ILR require 7 bits and 6 bytes for keeping track of the loop increment (maximum 128) and ID of the address of the inner striding load.

> **中文翻译：** 支持 DVR 的硬件结构仅产生 1139 字节的开销。 32 条目步幅检测器需要 460 字节：每个条目需要 48 位用于加载 PC，48 位用于前一个内存地址，16 位用于步距，2 位用于饱和计数器，1 位用于最内层检测。 VRAT 是一个 16 条目表（288 字节）：每个条目具有 16 个寄存器标识符，每个寄存器标识符需要 9 位（以选择 128 个向量物理寄存器和 256 个整数物理寄存器之一）。 VIR 占用 86 个字节：128 位用于掩码，16 位已发出，16 位已执行，64 位 uop 和 imm，9×16 位用于目标，10×16 位用于 src1，10×16 位用于 src2。前端缓冲区需要 64 个字节用于 8 个微操作。 8 项再收敛堆栈需要 176 个字节：6 个字节用于 PC，每个 PC 需要 128 位掩码。 FLR和LCR分别只需要6字节和2字节； SBB 仅需要 1 位。循环限制检测器保存两个检查点（2×16×8 位用于寄存器 ID 映射）和两个用于比较和分支指令的寄存器，总共 48 个字节。污点跟踪器需要 16 位。对于 NDM，IR 和 ILR 需要 7 位和 6 个字节来跟踪循环增量（最大 128）和内部跨越负载的地址 ID。

## 5 EXPERIMENTAL SETUP / 5 实验设置

**Simulation Infrastructure.** We use Sniper 6.0 [18], an x86 simulator with its most detailed, cycle-level core model to simulate

> **中文翻译：** 模拟基础设施。我们使用 Sniper 6.0 [18]，这是一个 x86 模拟器，具有最详细的周期级核心模型来进行模拟

an aggressive 5-wide 350-entry ROB superscalar, out-of-order processor. The configuration of the core, the key microarchitectural structures of which are inspired by Intel Ice Lake processors [36], is provided in Table 1 [28]. A hardware stride prefetcher is always enabled at the L1-D cache level. Additionally, there are 24 MSHRs to keep track of outstanding misses from L1-D. The branch predictor is the 8 KB TAGE-SC-L from the 2016 CBP [83].

> **中文翻译：** 一个激进的 5 宽 350 项 ROB 超标量乱序处理器。表 1 [28] 提供了核心的配置，其关键微架构结构受到 Intel Ice Lake 处理器 [36] 的启发。硬件跨步预取器始终在 L1-D 缓存级别启用。此外，还有 24 个 MSHR 来跟踪 L1-D 的突出失误。分支预测器是 2016 年 CBP 中的 8 KB TAGE-SC-L [83]。

**Table 2: Graph inputs used for the GAP suite [12].**

> **中文翻译：** 表 2：用于 GAP 套件 [12] 的图形输入。

|Input|# Nodes<br>(in Millions)|# Edges<br>(in Millions)|LLC MPKI|
|---|---|---|---|
|Kron(KR)|134.2|2111.6|19|
|LiveJournal(LJN)|4.8|69.0|21|
|Orkut(ORK)|3.1|1930.3|18|
|Twitter(TW)|61.6|1468.4|61|
|Urand(UR)|134.2|2147.4|32|

**中文表格：**

|输入|节点<br（单位：百万）|边<br（以百万为单位）|LLC MPKI|
|---|---|---|---|
|克朗(KR)|134.2|2111.6|19|
|LiveJournal(LJN)|4.8|69.0|21|
|奥库特(ORK)|3.1|1930.3|18|
|推特(台湾)|61.6|1468.4|61|
|乌兰德(UR)|134.2|2147.4|32|

**Benchmarks.** We evaluate a total of 13 benchmarks from the graph analytics, database, and HPC domains featuring complex address-calculation patterns for a chain of indirect memory accesses. Five of the benchmarks are taken from the GAP benchmark suite [12]: Betweenness Centrality (bc), Breadth-First Search (bfs), Connected Components (cc), PageRank (pr), and Single-Source Shortest Path (sssp). Eight benchmarks, namely Camel, Graph500, Hashjoin with two and eight hashes (HJ2 and HJ8), Kangaroo, NAS-CG, NAS-IS, and RandomAccess, are primarily from the database and HPC domains; these benchmarks have been extensively used by prior work [2, 3, 67, 88, 89], and we collectively call them hpc-db (high-performance computing and databases benchmark). Table 2 describes graph inputs; LLC MPKI shows the number of misses per kilo instructions aggregated over the five benchmarks for each input on our baseline OoO core. We use the region-of-interest (ROI) marker utility in Sniper to skip the initialization phase for each benchmark and simulate the next representative 500 M instructions.

> **中文翻译：** 基准。我们评估了来自图分析、数据库和 HPC 领域的总共 13 个基准测试，这些基准测试具有用于间接内存访问链的复杂地址计算模式。其中五个基准来自 GAP 基准套件 [12]：介数中心性 (bc)、广度优先搜索 (bfs)、连通组件 (cc)、PageRank (pr) 和单源最短路径 (sssp)。八个基准测试，即 Camel、Graph500、具有两个和八个哈希值的 Hashjoin（HJ2 和 HJ8）、Kangaroo、NAS-CG、NAS-IS 和 RandomAccess，主要来自数据库和 HPC 领域；这些基准已被之前的工作广泛使用[2,3,67,88,89]，我们统称为hpc-db（高性能计算和数据库基准）。表 2 描述了图形输入； LLC MPKI 显示了我们基线 OoO 内核上每个输入在五个基准上聚合的每千指令的未命中数。我们使用 Sniper 中的感兴趣区域 (ROI) 标记实用程序来跳过每个基准测试的初始化阶段，并模拟接下来的代表性 500 M 指令。

## 6 EVALUATION / 6 评估

We evaluate the following runahead techniques relative to our baseline OoO core:

> **中文翻译：** 我们相对于我们的基准 OoO 核心评估以下前瞻执行技术：

- Precise Runahead Execution (PRE) [69]: The state-of-the-art runahead technique that selectively executes only the chain of instructions leading to long-latency loads, and recycles register-file and issue-queue resources dynamically to avoid pipeline flushes.

> **中文翻译：** - 精确先行执行（PRE）[69]：最先进的先行技术，有选择地仅执行导致长延迟加载的指令链，并动态回收寄存器文件和问题队列资源以避免管道刷新。

- Indirect Memory Prefetcher (IMP) [98]: The indirect memory prefetcher, that works at L1 D-cache level and prefetches indirect memory accesses originating from striding access patterns.

> **中文翻译：** - 间接内存预取器（IMP）[98]：间接内存预取器，在 L1 D 缓存级别工作，预取源自跨步访问模式的间接内存访问。

- Vector Runahead (VR): The first vector-runahead mechanism proposed by Naithani et al. [67].

> **中文翻译：** - Vector Runahead (VR)：Naithani 等人提出的第一个向量运行机制。 [67]。

- Decoupled Vector Runahead (DVR).

> **中文翻译：** - 解耦向量运行 (DVR)。

- Oracle: A hypothetical technique that knows all memory accesses in advance, and prefetches them at the appropriate point in time to avoid stalling.

> **中文翻译：** - Oracle：一种假设的技术，可以提前知道所有内存访问，并在适当的时间点预取它们以避免停滞。

## 6.1 Performance / 6.1 性能

Figure 7 reports normalized performance for each technique on every benchmark-input combination. PRE rarely yields more than

> **中文翻译：** 图 7 报告了每种技术在每个基准输入组合上的标准化性能。 PRE 的产量很少超过

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0009-02.png)

**Figure 7: Performance for PRE, VR, DVR and Oracle normalized to a baseline OoO core.** _DVR achieves_ 2.4× _higher performance (and up to_ 6.4× _) compared to a baseline OoO core._

> **中文翻译：** 图 7：PRE、VR、DVR 和 Oracle 的性能标准化为基准 OoO 核心。与基准 OoO 内核相比，DVR 的性能提高了 2.4 倍（最高可达 6.4 倍）。

negligible performance improvements (with Camel and NAS-IS as exceptions). IMP performs better than PRE as it can detect simple indirect patterns in benchmarks such as cc, Camel, and NAS-IS. However, it cannot prefetch indirect accesses for other benchmarks with more complex address calculation patterns. Vector Runahead manages slightly more (1.2× harmonic mean) because it is able to follow, reorder and vectorize the chains. Still, both PRE and VR suffer on large cores. The ROB rarely fills up, and even though there is still potential performance to be gained, the fact that neither PRE nor VR often reach their trigger condition limits their speedup. This is especially pronounced on the GAP benchmarks, where frequent branch mispredictions imply that the reorder buffer rarely reaches full utilization before the misprediction is discovered. This is the reason why IMP, which is detached from the core size and works at L1 D-cache level, performs better than VR for benchmarks such as cc_KR and cc_TW. In some cases where Vector Runahead is triggered, it decreases performance because of its inaccuracy (e.g., bfs on the UR dataset): when inner loops are short, the lack of DVR’s Discovery Mode evicts useful data from the cache and wastes DRAM bandwidth. DVR often yields close to Oracle-level performance; it is more proactive in generating prefetches than VR and PRE, and achieves a 2.4× average speedup and 6.4× maximum.

> **中文翻译：** 性能改进可以忽略不计（Camel 和 NAS-IS 除外）。 IMP 的性能优于 PRE，因为它可以检测 cc、Camel 和 NAS-IS 等基准测试中的简单间接模式。但是，它无法为具有更复杂地址计算模式的其他基准预取间接访问。 Vector Runahead 的管理能力稍强（1.2×调和平均值），因为它能够跟踪、重新排序和向量化链。尽管如此，PRE 和 VR 在大内核上都会受到影响。 ROB 很少填满，尽管仍然有潜在的性能提升，但 PRE 和 VR 都不经常达到触发条件，这一事实限制了它们的加速。这在 GAP 基准测试中尤其明显，其中频繁的分支错误预测意味着在发现错误预测之前，重排序缓冲区很少达到充分利用。这就是为什么 IMP（与核心大小无关并在 L1 D-cache 级别工作）在 ccKR 和 ccTW 等基准测试中表现优于 VR 的原因。在触发 Vector Runahead 的某些情况下，它会因其不准确而降低性能（例如，UR 数据集上的 bfs）：当内部循环较短时，缺乏 DVR 的发现模式会从缓存中逐出有用数据并浪费 DRAM 带宽。 DVR 通常会产生接近 Oracle 级别的性能；它比 VR 和 PRE 更主动地生成预取，并实现了 2.4 倍的平均加速和 6.4 倍的最大加速。

Granted, there are still some workloads where DVR does not reach the full potential of a perfect Oracle, since it is not given full knowledge of the future or unlimited resources. In some cases (NAS-CG and NAS-IS), the workload is so simple that looking ahead only 128 elements into the future is insufficient to hide the full memory latency on such a large core: wider 256-element DVR units would achieve the higher performance of the Oracle, at the expense of a larger VRAT and more physical vector registers being required to be mapped simultaneously. In others, the memory-level parallelism is more difficult to find. This is particularly pronounced on workloads running the UR graph, where vertices are uniformly smaller than the 128-edge-element target, used by DVR within inner loops to generate MLP, unlike the power-law graphs (KR and Graph 500) which spend more time in highly populated vertices. As we shall see, Nested Vector Runahead mitigates this issue partially but still suffers from timeliness due to the complex dependencies.

> **中文翻译：** 当然，在某些工作负载中，DVR 仍无法充分发挥完美 Oracle 的潜力，因为它没有充分了解未来或无限的资源。在某些情况下（NAS-CG 和 NAS-IS），工作负载非常简单，仅展望未来 128 个元素不足以隐藏如此大核心上的全部内存延迟：更宽的 256 元素 DVR 单元将实现 Oracle 的更高性能，但代价是需要同时映射更大的 VRAT 和更多物理向量寄存器。在其他情况下，内存级并行性更难找到。这在运行 UR 图的工作负载上尤其明显，其中顶点均小于 128 个边元素目标，DVR 在内部循环中使用该目标来生成 MLP，这与幂律图（KR 和 Graph 500）不同，幂律图（KR 和 Graph 500）在高度密集的顶点上花费更多时间。正如我们将看到的，Nested Vector Runahead 部分缓解了这个问题，但由于复杂的依赖关系，时效性仍然受到影响。

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0009-06.png)

**Figure 8: Breaking down DVR’s performance normalized to the baseline OoO: (1) Vector Runahead [67], (2) Offload triggers a vector-runahead subthread whenever a stride is detected, (3) Discovery Mode further improves prefetch accuracy, and (4) Nested Runahead Mode completes DVR by further increasing memory-level parallelism over short loops.**

> **中文翻译：** 图 8：将 DVR 的性能分解为基准 OoO：(1) Vector Runahead [67]，(2) 每当检测到跨步时，Offload 就会触发向量运行超前子线程，(3) 发现模式进一步提高预取精度，(4) 嵌套运行超前模式通过进一步提高短循环上的内存级并行性来完成 DVR。

## 6.2 Performance Breakdown / 6.2 性能分解

Figure 8 shows how the constituent parts of DVR contribute to the overall performance gain. Offloading Vector Runahead to a subthread, and thus allowing it to run more proactively than just on a full ROB, gives large benefits on its own: from 1.2× with a base Vector Runahead to almost 1.5× here. Indeed, the fact that the base Vector Runahead is out-of-order and the offloaded DVR is in-order is barely relevant when it comes to performance: each scalar-equivalent instruction in DVR does so much work, and brings in so many (vectorized gather) loads that there is no need for full out-of-order execution in the vector-runahead subthread.

> **中文翻译：** 图 8 显示了 DVR 的组成部分如何提高整体性能。将 Vector Runahead 卸载到子线程，从而使其比仅在完整 ROB 上更主动地运行，本身就带来了巨大的好处：从基本 Vector Runahead 的 1.2 倍到此处的近 1.5 倍。事实上，基本 Vector Runahead 是乱序的，而卸载的 DVR 是有序的，这一事实在性能方面几乎没有关系：DVR 中的每个标量等效指令都做了很多工作，并带来了如此多的（向量化收集）负载，因此不需要在向量 runahead 子线程中完全乱序执行。

Adding Discovery Mode particularly benefits bc, bfs and sssp; the over-fetching that vector-runahead techniques otherwise cause results in enough cache pollution and bandwidth wastage for the more accurate Discovery Mode to win out. Still, it is a double-edged sword on cc and pr, where the wrong-path execution triggered by DVR without Discovery Mode happens to bring in the correct data despite being out-of-bounds, as each outer loop generates only sequential values for the inner loop, unlike bc, bfs and sssp. Still, the full DVR technique, completed with the addition of Nested Runahead Mode, is uniformly best, because it can most effectively generate MLP far into the future even for short inner loops.

> **中文翻译：** 添加发现模式特别有利于 bc、bfs 和 sssp；否则，向量前瞻执行技术的过度获取会导致足够的缓存污染和带宽浪费，从而使更准确的发现模式获胜。尽管如此，它对于 cc 和 pr 来说仍然是一把双刃剑，在没有发现模式的情况下由 DVR 触发的错误路径执行尽管越界，但仍然会引入正确的数据，因为每个外循环只为内循环生成顺序值，这与 bc、bfs 和 sssp 不同。尽管如此，通过添加嵌套运行模式完成的完整 DVR 技术仍然是最好的，因为即使对于较短的内部循环，它也可以最有效地生成远期的 MLP。

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0010-02.png)

**Figure 9: Memory-level parallelism, in terms of MSHRs used per cycle on average, for DVR and VR compared to the baseline OoO core.** _DVR generates significantly more parallel outstanding memory accesses._

> **中文翻译：** 图 9：DVR 和 VR 与基准 OoO 内核相比的内存级并行性（以平均每个周期使用的 MSHR 计）。 DVR 生成显着更多的并行未完成内存访问。

## 6.3 Memory-Level Parallelism / 6.3 内存级并行性

The secrets of DVR’s success are that it generates far more overlapping memory accesses than competing techniques. In Figure 9, we see that the number of outstanding requests for the out-of-order core in the data cache are less than four on average, with DVR generating more than ten at a time, on average per cycle, by comparison. The simplest workloads (pr and those in hpd-db) have fewer branch mispredicts, and so achieve higher raw memory-level parallelism even if the speedups are typically higher in the more complex workloads. Even though DVR itself does not suffer significantly from branch mispredicts (its simple in-order pipeline squashes them extremely early, and its coarse form of speculative loop parallelism means branches across loop iterations do not form chains that cause all instructions later in program order to be squashed), the main thread does, and so DVR naturally ends up looking less far ahead, and overlapping fewer accesses.

> **中文翻译：** DVR 成功的秘诀在于，它比竞争技术产生更多的重叠内存访问。在图 9 中，我们看到数据缓存中乱序核心的未完成请求数量平均不到 4 个，相比之下，DVR 平均每个周期一次生成的请求数量超过 10 个。最简单的工作负载（pr 和 hpd-db 中的工作负载）的分支错误预测较少，因此即使在更复杂的工作负载中加速通常更高，也能实现更高的原始内存级别并行性。尽管 DVR 本身并没有受到分支错误预测的严重影响（其简单的有序管道极早地挤压它们，并且其粗略形式的推测循环并行性意味着跨循环迭代的分支不会形成导致程序顺序中后面的所有指令被挤压的链），但主线程会这样做，因此 DVR 自然最终会看起来不那么遥远，并且重叠的访问也更少。

## 6.4 Effectiveness / 6.4 有效性

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0010-07.png)

**Figure 10: Accuracy and Coverage: number of off-chip memory accesses for VR and DVR normalized to OoO, and fraction of memory accesses in normal versus runahead mode.** _DVR successfully prefetches DRAM accesses, converting them into on-chip cache hits when the program subsequently accesses them in normal mode._

> **中文翻译：** 图 10：准确度和覆盖范围：标准化为 OoO 的 VR 和 DVR 片外存储器访问次数，以及正常模式与前瞻执行模式下的存储器访问比例。 DVR 成功预取 DRAM 访问，当程序随后在正常模式下访问它们时，将它们转换为片上缓存命中。

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0010-09.png)

**Figure 11: Timeliness: fraction of total prefetched cachelines in runahead mode for which the data is present in the L1-D, L2 and L3 caches during normal mode; ‘Off-chip’ represents either the cachelines prefetched incorrectly or the cache lines for which the data is still being transferred from memory.**

> **中文翻译：** 图 11：及时性：在正常模式下数据存在于 L1-D、L2 和 L3 缓存中的前瞻执行模式下预取缓存行总数的比例； “片外”表示缓存行预取不正确，或者数据仍在从内存传输的缓存行。

Here we analyze to what extent DVR is successful at generating accurate, timely, comprehensive prefetches.

> **中文翻译：** 在这里，我们分析 DVR 在多大程度上成功地生成准确、及时、全面的预取。

**Accuracy and Coverage.** Figure 10 shows both the total number of main memory accesses performed, and the fraction within the main thread and runahead mode or subthread. Both DVR and VR are given for comparison, relative to the same out-of-order baseline. DVR is extremely accurate because of the Discovery Mode. By contrast, Vector Runahead can over-fetch by over 2×, because it lacks loop-length analysis.

> **中文翻译：** 准确性和覆盖范围。图 10 显示了执行的主内存访问总数，以及主线程和前瞻执行模式或子线程内的比例。 DVR 和 VR 都是相对于相同的乱序基线进行比较的。由于发现模式，DVR 非常准确。相比之下，Vector Runahead 可以超取超过 2 倍，因为它缺乏循环长度分析。

As well as being more accurate, DVR also covers far more of each application, due to triggering more eagerly, and because Nested Mode can handle far more complex indirection.

> **中文翻译：** 除了更准确之外，DVR 还涵盖了每个应用程序的更多内容，因为触发更加急切，而且嵌套模式可以处理更复杂的间接。

**Timeliness.** Figure 11 shows how timely the prefetches are in DVR, in terms of the access latency observed by the main thread. Most cache lines are in the L1 D-cache when the main thread accesses them, with only a few evicted to higher cache levels. This is because the combination of the Discovery and Nested Modes allows DVR to generate very fine-grained memory-level parallelism, meaning that even though we are bringing in hundreds of entries at once, we can synchronize with the main thread so that they are accessed shortly after. Still, a consistent 10–20 percent of accesses observe a latency higher than the last-level cache. When interpreted in correspondence with Figure 10, we see that this is not because of inaccuracy. Rather, it is because the prefetches are too late. Because DVR overlaps with the main thread’s execution, and especially because Discovery and Nested Modes can delay the start of vectorization, many earlier accesses in a single runahead iteration may overlap with those same accesses in the main thread. This is a significant (if difficult to avoid) reason why the Oracle, which pays no such overheads for discovering future addresses, achieves better performance in some cases, as previously reported in Figure 7.

> **中文翻译：** 时效性。图 11 显示了 DVR 中预取的及时性（根据主线程观察到的访问延迟）。当主线程访问它们时，大多数缓存行都位于 L1 D 缓存中，只有少数被逐出到更高的缓存级别。这是因为发现模式和嵌套模式的组合允许 DVR 生成非常细粒度的内存级并行性，这意味着即使我们一次引入数百个条目，我们也可以与主线程同步，以便很快就可以访问它们。尽管如此，仍然有 10-20% 的访问观察到延迟高于最后一级缓存。当根据图 10 进行解释时，我们发现这并不是因为不准确。相反，这是因为预取太晚了。由于 DVR 与主线程的执行重叠，特别是因为发现和嵌套模式可以延迟向量化的开始，因此单个超前迭代中的许多早期访问可能与主线程中的相同访问重叠。这是 Oracle 无需为发现未来地址支付此类开销而在某些情况下实现更好性能的一个重要（如果难以避免）原因，如图 7 中先前报告的那样。

## 6.5 Core Size Sensitivity Analysis / 6.5 核心规模敏感性分析

Figure 12 reports performance for DVR as a function of ROB size normalized to our baseline OoO core with 350-entry ROB. In contrast to VR which yields diminishing performance benefits with increasing ROB size, as previously reported in Figure 2, the performance boost offered by DVR holds on. In contrast to VR which is triggered upon a full ROB, DVR operates in a decoupled manner from the main thread, significantly boosting performance by continuously vectorizing and prefetching future chains of dependent loads. When we scale all the back-end structures — in proportion to the ROB — the performance of DVR relative to the OoO baseline

> **中文翻译：** 图 12 将 DVR 的性能报告为 ROB 大小的函数，该大小标准化为具有 350 个条目 ROB 的基准 OoO 核心。 VR 的性能优势会随着 ROB 大小的增加而递减（如图 2 所示），与此相反，DVR 提供的性能提升持续存在。与在完整 ROB 上触发的 VR 不同，DVR 以与主线程解耦的方式运行，通过连续向量化和预取未来的相关负载链来显着提高性能。当我们按比例缩放所有后端结构时（与 ROB 成比例），DVR 的性能相对于 OoO 基线

![](Decoupled_Vector_Runahead_assets/Decoupled_Vector_Runahead.pdf-0011-02.png)

**Figure 12: Performance of DVR with increasing ROB size, relative to our baseline OoO core with 350-entry ROB.** _The performance gains delivered by DVR continue to increase despite the large size of the ROB._

> **中文翻译：** 图 12：相对于具有 350 个条目 ROB 的基准 OoO 核心，随着 ROB 大小的增加，DVR 的性能。尽管 ROB 尺寸很大，但 DVR 带来的性能提升仍在继续增加。

with 350-entry ROB is 1.9 ×, 2.2 ×, 2.2 ×, 2.4 ×, and 2.5 × higher for the cores with the ROB sizes of 128, 192, 224, 350, and 512 entries.

> **中文翻译：** 对于 ROB 大小为 128、192、224、350 和 512 条目的内核，具有 350 条目 ROB 的内核分别高出 1.9 ×、2.2 ×、2.2 ×、2.4 × 和 2.5 ×。

## 7 RELATED WORK / 7 相关工作

Decoupled Vector Runahead is both a helper thread [20, 44], and a runahead execution [66] according to the categories from Mittal et al. [59] and Falsafi et al. [32].

> **中文翻译：** 根据 Mittal 等人的分类，解耦的 Vector Runahead 既是一个辅助线程 [20, 44]，又是一个 runahead 执行 [66]。 [59] 和 Falsafi 等人。 [32]。

## 7.1 Helper Threads and Precomputation / 7.1 辅助线程与预计算

Helper threads perform work to assist the performance of a main thread. Athanasaki et al. [8] perform speculative precomputation on simultaneous multithreads. Wang et al. [93] run helper threads via context switching, removing the need for explicit SMT capabilities. SSMT [20] introduced hardware support specifically for helper threads, rather than SMT generically, and runs microthreads alongside the main core, via a buffer that stores hand-generated micro-ops.

> **中文翻译：** 辅助线程执行工作以辅助主线程的性能。阿萨纳萨基等人。 [8] 在同时多线程上执行推测性预计算。王等人。 [93]通过上下文切换运行辅助线程，消除了对显式 SMT 功能的需要。 SSMT [20] 引入了专门针对辅助线程的硬件支持，而不是一般的 SMT，并通过存储手动生成的微操作的缓冲区与主核心一起运行微线程。

Slice Processors [60] identify cache misses to precompute address calculation on a parallel thread. Dependence-graph computation [7, 82] executes the slices on separate hardware. Lau et al. [53] introduces a small core by not only duplicating the execution hardware, but some of the core front-end as well, to run speculative threads. DeSC [37] decouples address calculation and load-value usage into two separate devices.

> **中文翻译：** 切片处理器[60]识别缓存未命中以在并行线程上预先计算地址计算。依赖图计算 [7, 82] 在单独的硬件上执行切片。刘等人。 [53]引入了一个小核心，不仅复制了执行硬件，还复制了一些核心前端，以运行推测线程。 DeSC [37] 将地址计算和加载值使用解耦到两个单独的设备中。

Kim et al. [45] generate helper threads automatically in the compiler. Ganusov and Burtscher [35] emulate hardware prefetchers on helper threads. Speculative Precomputation [25] allows helper threads to spawn their own helper threads to handle chain dependencies.

> **中文翻译：** 金等人。 [45]在编译器中自动生成辅助线程。 Ganusov 和 Burtscher [35] 在辅助线程上模拟硬件预取器。推测性预计算 [25] 允许辅助线程生成自己的辅助线程来处理链依赖性。

None of this prior work reorders and vectorizes the code in the helper thread to prefetch dependent memory operations far into the future instruction stream, and typically it requires software support instead of being fully microarchitectural.

> **中文翻译：** 这些先前的工作都没有对辅助线程中的代码进行重新排序和向量化，以将依赖的内存操作预取到未来的指令流中，并且通常需要软件支持，而不是完全的微架构。

or not to enter runahead can reduce the number of executed instructions, while keeping the performance benefits intact. Hashemi et al. [40] filter and buffer dependency chains to improve performance. Precise Runahead [70] both filters instructions and avoids throwing away correct instructions that fit inside the ROB. Branch Runahead [78] uses a light dependency chain executed continuously to assist the branch predictor. Bringing runahead together with vectorization was the key idea of Vector Runahead [67, 68] (Section 2.3).

> **中文翻译：** 或不进入前瞻执行可以减少执行指令的数量，同时保持性能优势不变。哈希米等人。 [40] 过滤器和缓冲区依赖链以提高性能。 Precise Runahead [70] 既可以过滤指令，又可以避免丢弃适合 ROB 内部的正确指令。 Branch Runahead [78] 使用连续执行的轻依赖链来辅助分支预测器。将 runahead 与向量化结合起来是 Vector Runahead [67, 68]（第 2.3 节）的关键思想。

Helper threads have been combined with (scalar) runahead execution [80]. MLP-aware runahead threads [27] only initiate execution with far-distance MLP. Ramirez et al. [79] dynamically calculate the offset at which runahead thread should run. Continuous Runahead [39] offloads simple address patterns to a core at the last-level cache controller, and runs them continuously. As continuous runahead can only prefetch chains leading to independent memory accesses, EMC [38], another near-memory core, prefetches dependent cache misses. Both continuous runahead and EMC are in-order, like DVR, but due to a lack of vectorization and instruction reordering, they cannot deliver high coverage and performance like DVR.

> **中文翻译：** 辅助线程已与（标量）提前执行相结合[80]。 MLP 感知的超前线程 [27] 仅使用远距离 MLP 启动执行。拉米雷斯等人。 [79]动态计算超前线程应该运行的偏移量。 Continuous Runahead [39] 将简单的地址模式卸载到最后一级缓存控制器的核心，并连续运行它们。由于连续前瞻执行只能预取导致独立内存访问的链，因此另一个近内存核心 EMC [38] 会预取相关的缓存未命中。连续前瞻执行和 EMC 都是有序的，如 DVR，但由于缺乏向量化和指令重新排序，它们无法提供像 DVR 那样的高覆盖率和性能。

## 7.2 Runahead Techniques / 7.2 前瞻执行技术

Runahead execution [29, 65, 66] was proposed as an alternative to large reorder buffers, allowing execution to continue after a longlatency load by removing the blocking instruction from the reorder buffer and continuing to transiently execute other instructions. Mutlu et al. [62, 64] showed that dynamically choosing whether

> **中文翻译：** 超前执行[29,65,66]被提出作为大型重排序缓冲区的替代方案，通过从重排序缓冲区中删除阻塞指令并继续瞬时执行其他指令，允许在长延迟加载后继续执行。穆特鲁等人。 [62, 64]表明动态选择是否

## 7.3 Auto-Vectorization and SW Reordering / 7.3 自动向量化与软件重排

DVR can be seen as a type of speculative vectorization [10, 52, 55, 57, 74, 76, 77, 86], albeit one that is generated microarchitecturally, that does not seek to maintain guaranteed correctness of its transient workload, and which overlaps far more independent loads than a single vector at a time. Likewise, it can be seen as a type of hardwaregenerated, compute-optimized (via vectorization) software pipelining [21, 47, 81, 90, 92], or software prefetching [3, 4, 17, 21, 61, 89] in that it reorders loads to overlap them.

> **中文翻译：** DVR 可​​以被视为一种推测向量化 [10, 52, 55, 57, 74, 76, 77, 86]，尽管它是在微架构上生成的，但它并不寻求保证其瞬态工作负载的正确性，并且一次重叠的独立负载比单个向量要多得多。同样，它可以被视为一种硬件生成、计算优化（通过向量化）的软件流水线 [21, 47, 81, 90, 92] 或软件预取 [3, 4, 17, 21, 61, 89]，因为它对负载进行重新排序以使其重叠。

## 7.4 Architecturally Visible Prefetching / 7.4 架构可见的预取

Prefetching the most complex memory access patterns has traditionally been the preserve of compiler- or hand-targeted hardware. The Event-Triggered Programmable Prefetcher [2] offloads and overlaps many memory accesses like DVR, to hide the latencies of dependent chains. However, it uses compiler- or hand-generated thread-level parallelism, and runs on a sea of small, dedicated cores.

> **中文翻译：** 预取最复杂的内存访问模式传统上一直是编译器或手动目标硬件的职责。事件触发的可编程预取器 [2] 卸载并重叠许多内存访问（如 DVR），以隐藏依赖链的延迟。然而，它使用编译器或手工生成的线程级并行性，并在大量小型专用核心上运行。

Harbinger instructions [5], Guided-Region Prefetching [94] and RnR [99] generate hints inside programs to give to prefetchers. Prodigy [88] and the Graph Prefetcher [1] are configured with a set of dependent-chain patterns typical to graph workloads. Other prefetchers are configured with the indirection patterns of arrays [19] or linked structures [24, 49]. Such hints may configure the entire memory hierarchy [97].

> **中文翻译：** Harbinger 指令 [5]、Guided-Region Prefetching [94] 和 RnR [99] 在程序内生成提示以提供给预取器。 Prodigy [88] 和 Graph Prefetcher [1] 配置了一组图形工作负载典型的依赖链模式。其他预取器配置有数组 [19] 或链接结构 [24, 49] 的间接模式。这样的提示可以配置整个内存层次结构[97]。

Fetcher units [41, 48, 50, 51, 56, 73, 100] are configured with the memory access pattern, but directly access data rather than prefetching it, reducing work repetition at the expense of requiring stricter ordering guarantees to preserve correctness.

> **中文翻译：** 获取器单元[41、48、50、51、56、73、100]配置有内存访问模式，但直接访问数据而不是预取数据，减少了工作重复，但代价是需要更严格的排序保证以保持正确性。

## 7.5 Microarchitectural Prefetchers / 7.5 微体系结构预取器

Stride prefetchers [22, 23], for repeated patterns in addresses such as sequential walks through arrays, are endemic in commercial systems [9]. The recent research literature focuses on improving their coverage, performance and selectivity [11, 15, 46, 58, 71, 84].

> **中文翻译：** 针对地址中的重复模式（例如顺序遍历数组）的跨步预取器 [22, 23] 在商业系统中很常见 [9]。最近的研究文献侧重于提高其覆盖范围、性能和选择性[11,15,46,58,71,84]。

More recently, temporal-history prefetchers [42, 43, 72, 95, 96], which store and repeat observed patterns, have become practical enough for deployment in Arm processors [34]. Pythia [14] considers more than just the PC to index predictors, by using reinforcement learning to select the relevant characteristics. Hermes [13] predicts whether data will be cached or off-chip, to avoid waiting for the cache miss before accessing off-chip memory. Shi et. al [85] correlate address patterns via machine learning. The complex datadependent chains within big-data workloads that DVR targets are unsuited to address correlation, given their lack of regular address pattern, or temporal reuse over even gigabytes of data [2].

> **中文翻译：** 最近，存储和重复观察到的模式的时间历史预取器 [42,43,72,95,96] 已经变得足够实用，可以在 Arm 处理器中部署 [34]。 Pythia [14] 不仅仅考虑 PC 来索引预测变量，还通过使用强化学习来选择相关特征。 Hermes [13] 预测数据将被缓存还是片外，以避免在访问片外内存之前等待缓存未命中。石等。 al [85]通过机器学习关联地址模式。 DVR 目标的大数据工作负载中复杂的数据依赖链不适合解决相关性问题，因为它们缺乏常规地址模式，或者甚至在千兆字节数据上的时间重用 [2]。

Cooksey et al. [26] propose a ‘content-directed’ prefetcher designed to pick up possible pointers within arrays, with others attempting to reduce overfetch rates via compiler input [5, 30]. IMP is successful at simple indirection patterns [98] but does not scale to graph or database workloads [67]. Takayashiki et al. [87] generate similar simple stride-indirects by observing vector gather instructions. The Bouquet of Prefetchers [75] predicts which prefetcher is best to use for each PC address.

> **中文翻译：** 库克西等人。 [26] 提出了一种“内容导向”预取器，旨在拾取数组中可能的指针，其他人则试图通过编译器输入来降低超取率 [5, 30]。 IMP 在简单的间接模式方面取得了成功 [98]，但无法扩展到图形或数据库工作负载 [67]。高屋敷等人。 [87]通过观察向量聚集指令生成类似的简单跨步间接。 Bouquet of Prefetchers [75] 预测哪个预取器最适合每个 PC 地址。

## 8 CONCLUSION / 8 结论

Decoupled Vector Runahead offloads the runahead execution to a simple, in-order, SIMT, vector subthread that is initiated whenever the core detects an indirect memory access pattern. Unlike prior runahead techniques, DVR does not wait for the reorder buffer to stall, and by discovering the loop bound at runtime, it can adjust the degree of vectorization to better suit application characteristics. DVR generates prefetches from multiple invocations of a loop when the discovered degree of vectorization for one invocation is not sufficient to achieve high memory-level parallelism. DVR incurs minimal hardware overhead of 1139 bytes.

> **中文翻译：** 解耦向量运行将超前执行卸载到一个简单、有序、SIMT 的向量子线程，每当核心检测到间接内存访问模式时，该子线程就会启动。与之前的前瞻执行技术不同，DVR 不会等待重排序缓冲区停止，并且通过在运行时发现循环界限，它可以调整向量化程度以更好地适应应用程序特性。当发现的一次调用的向量化程度不足以实现高内存级别并行性时，DVR 会从循环的多次调用中生成预取。 DVR 的硬件开销最小为 1139 字节。

The benefits of reordering-based runahead over invalidation runaheads will usher in a new era of processors with the latency insensitivity of GPUs while maintaining the programmability and single-threaded performance of CPUs. The potential of near-oracle performance for even the trickiest graph workloads is too tempting to leave on the table.

> **中文翻译：** 基于重新排序的提前运行相对于失效提前运行的优势将开创处理器的新时代，具有 GPU 的延迟不敏感性，同时保持 CPU 的可编程性和单线程性能。即使对于最棘手的图形工作负载来说，接近预言机的性能潜力也很诱人，不容忽视。

## ACKNOWLEDGMENTS / 致谢

We thank the reviewers for their valuable feedback. This work is supported in part by the UGent-BOF-GOA grant No. 01G01421, the Research Foundation Flanders (FWO) grant No. G018722N, the European Research Council (ERC) Advanced Grant agreement No. 741097, and the Engineering and Physical Sciences Research Council (EPSRC) grant reference EP/W00576X/1. Additional data related to this publication is available on request from the lead author.

> **中文翻译：** 我们感谢审稿人的宝贵反馈。这项工作得到了 UGent-BOF-GOA 拨款号 01G01421、法兰德斯研究基金会 (FWO) 拨款号 G018722N、欧洲研究理事会 (ERC) 高级拨款协议号 741097 和工程与物理科学研究委员会 (EPSRC) 拨款参考号 EP/W00576X/1 的部分支持。与本出版物相关的其他数据可根据主要作者的要求提供。

## REFERENCES / 参考文献

- [1] Sam Ainsworth and Timothy M. Jones. 2016. Graph Prefetching Using Data Structure Knowledge. In _Proceedings of the 2016 International Conference on Supercomputing_ (Istanbul, Turkey) _(ICS ’16)_ . Association for Computing Machinery, New York, NY, USA, Article 39, 11 pages. https://doi.org/10.1145/2925426. 2926254

- [2] Sam Ainsworth and Timothy M. Jones. 2018. An Event-Triggered Programmable Prefetcher for Irregular Workloads. In _Proceedings of the Twenty-Third International Conference on Architectural Support for Programming Languages and Operating Systems_ (Williamsburg, VA, USA) _(ASPLOS ’18)_ . Association for Computing Machinery, New York, NY, USA, 578–592. https://doi.org/10.1145/3173162.

3173189

- [3] Sam Ainsworth and Timothy M. Jones. 2019. Software Prefetching for Indirect Memory Accesses: A Microarchitectural Perspective. _ACM Transactions on Computer Systems_ 36, 3, Article 8 (jun 2019), 34 pages. https://doi.org/10.1145/ 3319393

- [4] Sam Ainsworth and Timothy M. Jones. 2020. Prefetching in Functional Languages. In _Proceedings of the 2020 ACM SIGPLAN International Symposium on Memory Management_ (London, UK) _(ISMM ’20)_ . Association for Computing Machinery, New York, NY, USA, 16–29. https://doi.org/10.1145/3381898.3397209

- [5] Hassan Al-Sukhni, Ian Bratt, and Daniel A. Connors. 2003. Compiler-Directed Content-Aware Prefetching for Dynamic Data Structures. In _Proceedings of the 12th International Conference on Parallel Architectures and Compilation Techniques (PACT ’03)_ . IEEE Computer Society, Los Alamitos, CA, USA, 91. https://doi.org/10.1109/PACT.2003.1238005

- [6] James Alfred Ang, Brian W. Barrett, Kyle Bruce Wheeler, and Richard C. Murphy. 2010. Introducing the graph 500. _Cray User’s Group (CUG)_ 19 (5 2010), 45–74. https://www.osti.gov/biblio/1014641

- [7] Murali Annavaram, Jignesh M. Patel, and Edward S. Davidson. 2001. Data Prefetching by Dependence Graph Precomputation. In _Proceedings of the 28th Annual International Symposium on Computer Architecture_ (Göteborg, Sweden) _(ISCA ’01)_ . Association for Computing Machinery, New York, NY, USA, 52–61. https://doi.org/10.1145/379240.379251

- [8] Evangelia Athanasaki, Nikos Anastopoulos, Kornilios Kourtis, and Nectarios Koziris. 2008. Exploring the Performance Limits of Simultaneous Multithreading for Memory Intensive Applications. _Journal of Supercomputing_ 44, 1 (apr 2008), 64–97. https://doi.org/10.1007/s11227-007-0149-x

- [9] Grant Ayers, Heiner Litz, Christos Kozyrakis, and Parthasarathy Ranganathan. 2020. Classifying Memory Access Patterns for Prefetching. In _Proceedings of the Twenty-Fifth International Conference on Architectural Support for Programming Languages and Operating Systems_ (Lausanne, Switzerland) _(ASPLOS ’20)_ . Association for Computing Machinery, New York, NY, USA, 513–526. https://doi.org/10.1145/3373376.3378498

- [10] Sara S. Baghsorkhi, Nalini Vasudevan, and Youfeng Wu. 2016. FlexVec: AutoVectorization for Irregular Loops. In _Proceedings of the 37th ACM SIGPLAN Conference on Programming Language Design and Implementation_ (Santa Barbara, CA, USA) _(PLDI ’16)_ . Association for Computing Machinery, New York, NY, USA, 697–710. https://doi.org/10.1145/2908080.2908111

- [11] Mohammad Bakhshalipour, Mehran Shakerinava, Pejman Lotfi-Kamran, and Hamid Sarbazi-Azad. 2019. Bingo Spatial Data Prefetcher. In _2019 IEEE International Symposium on High Performance Computer Architecture (HPCA ’19)_ . IEEE Computer Society, Los Alamitos, CA, USA, 399–411. https://doi.org/10.1109/ HPCA.2019.00053

- [12] Scott Beamer, Krste Asanović, and David Patterson. 2017. The GAP Benchmark Suite. arXiv:1508.03619 [cs.DC]

- [13] Rahul Bera, Konstantinos Kanellopoulos, Shankar Balachandran, David Novo, Ataberk Olgun, Mohammad Sadrosadat, and Onur Mutlu. 2022. Hermes: Accelerating Long-Latency Load Requests via Perceptron-Based Off-Chip Load Prediction. In _2022 55th IEEE/ACM International Symposium on Microarchitecture (MICRO-55)_ . IEEE Computer Society, Los Alamitos, CA, USA, 1–18. https://doi.org/10.1109/MICRO56248.2022.00015

- [14] Rahul Bera, Konstantinos Kanellopoulos, Anant Nori, Taha Shahroodi, Sreenivas Subramoney, and Onur Mutlu. 2021. Pythia: A Customizable Hardware Prefetching Framework Using Online Reinforcement Learning. In _MICRO-54: 54th Annual IEEE/ACM International Symposium on Microarchitecture_ (Virtual Event, Greece) _(MICRO ’21)_ . Association for Computing Machinery, New York, NY, USA, 1121–1137. https://doi.org/10.1145/3466752.3480114

- [15] Rahul Bera, Anant V. Nori, Onur Mutlu, and Sreenivas Subramoney. 2019. DSPatch: Dual Spatial Pattern Prefetcher. In _Proceedings of the 52nd Annual IEEE/ACM International Symposium on Microarchitecture_ (Columbus, OH, USA) _(MICRO ’52)_ . Association for Computing Machinery, New York, NY, USA, 531–544. https://doi.org/10.1145/3352460.3358325

- [16] Ulrik Brandes. 2001. A faster algorithm for betweenness centrality. _The Journal of Mathematical Sociology_ 25, 2 (2001), 163–177. https://doi.org/10.1080/0022250X. 2001.9990249

- [17] David Callahan, Ken Kennedy, and Allan Porterfield. 1991. Software Prefetching. In _Proceedings of the Fourth International Conference on Architectural Support for Programming Languages and Operating Systems_ (Santa Clara, California, USA) _(ASPLOS IV)_ . Association for Computing Machinery, New York, NY, USA, 40–52. https://doi.org/10.1145/106972.106979

- [18] Trevor E. Carlson, Wim Heirman, Stijn Eyerman, Ibrahim Hur, and Lieven Eeckhout. 2014. An evaluation of high-level mechanistic core models. _ACM Transactions on Architecture and Code Optimization_ 11, 3, Article 28 (aug 2014), 25 pages. https://doi.org/10.1145/2629677

- [19] Mustafa Cavus, Resit Sendag, and Joshua J. Yi. 2020. Informed Prefetching for Indirect Memory Accesses. _ACM Transactions on Architecture and Code Optimization_ 17, 1, Article 4 (mar 2020), 29 pages. https://doi.org/10.1145/ 3374216

- [20] Robert S. Chappell, Jared Stark, Sangwook P. Kim, Steven K. Reinhardt, and Yale N. Patt. 1999. Simultaneous Subordinate Microthreading (SSMT). In _Proceedings of the 26th Annual International Symposium on Computer Architecture_ (Atlanta, Georgia, USA) _(ISCA ’99)_ . IEEE Computer Society, Los Alamitos, CA, USA, 186–195. https://doi.org/10.1145/300979.300995

- [21] Shimin Chen, Anastassia Ailamaki, Phillip B. Gibbons, and Todd C. Mowry. 2007. Improving Hash Join Performance through Prefetching. _ACM Transactions on Database Systems_ 32, 3 (aug 2007), 17–es. https://doi.org/10.1145/1272743. 1272747

- [22] Tien-Fu Chen and Jean-Loup Baer. 1992. Reducing Memory Latency via Nonblocking and Prefetching Caches. In _Proceedings of the Fifth International Conference on Architectural Support for Programming Languages and Operating Systems_ (Boston, Massachusetts, USA) _(ASPLOS V)_ . Association for Computing Machinery, New York, NY, USA, 51–61. https://doi.org/10.1145/143365.143486

- [23] Tien-Fu Chen and Jean-Loup Baer. 1995. Effective hardware-based data prefetching for high-performance processors. _IEEE Trans. Comput._ 44, 5 (May 1995), 609–623. https://doi.org/10.1109/12.381947

- [24] Seungryul Choi, Nicholas Kohout, Sumit Pamnani, Dongkeun Kim, and Donald Yeung. 2004. A General Framework for Prefetch Scheduling in Linked Data Structures and Its Application to Multi-chain Prefetching. _ACM Transactions on Computer Systems_ 22, 2 (may 2004), 214–280. https://doi.org/10.1145/986533. 986536

- [25] Jamison D. Collins, Hong Wang, Dean M. Tullsen, Christopher Hughes, YongFong Lee, Dan Lavery, and John P. Shen. 2001. Speculative Precomputation: Long-Range Prefetching of Delinquent Loads. In _Proceedings of the 28th Annual International Symposium on Computer Architecture_ (Göteborg, Sweden) _(ISCA ’01)_ . Association for Computing Machinery, New York, NY, USA, 14–25. https: //doi.org/10.1145/379240.379248

- [26] Robert Cooksey, Stephan Jourdan, and Dirk Grunwald. 2002. A Stateless, Content-directed Data Prefetching Mechanism. In _Proceedings of the 10th International Conference on Architectural Support for Programming Languages and Operating Systems_ (San Jose, California) _(ASPLOS X)_ . Association for Computing Machinery, New York, NY, USA, 279–290. https://doi.org/10.1145/605397.605427

- [27] Kenzo Van Craeynest, Stijn Eyerman, and Lieven Eeckhout. 2009. MLP-Aware Runahead Threads in a Simultaneous Multithreading Processor. In _High Performance Embedded Architectures and Compilers, Fourth International Conference, HiPEAC 2009, Paphos, Cyprus, January 25-28, 2009. Proceedings (Lecture Notes in Computer Science, Vol. 5409)_ . Springer Berlin Heidelberg, Berlin, Heidelberg, 110–124. https://doi.org/10.1007/978-3-540-92990-1_10

- [28] Dr. Ian Cutress. 2018. _Intel’s Architecture Day 2018: The future of core, Intel gpus, 10nm, and hybrid x86_ . AnandTech. https://www.anandtech.com/show/13699/ intel-architecture-day-2018-core-future-hybrid-x86

- [29] James Dundas and Trevor Mudge. 1997. Improving Data Cache Performance by Pre-Executing Instructions under a Cache Miss. In _Proceedings of the 11th International Conference on Supercomputing_ (Vienna, Austria) _(ICS ’97)_ . Association for Computing Machinery, New York, NY, USA, 68–75. https: //doi.org/10.1145/263580.263597

- [30] Eiman Ebrahimi, Onur Mutlu, and Yale N. Patt. 2009. Techniques for bandwidthefficient prefetching of linked data structures in hybrid prefetching systems. In _2009 IEEE 15th International Symposium on High Performance Computer Architecture_ . IEEE Computer Society, Los Alamitos, CA, USA, 7–17. https: //doi.org/10.1109/HPCA.2009.4798232

- [31] Jack Edmonds and Richard M. Karp. 1972. Theoretical Improvements in Algorithmic Efficiency for Network Flow Problems. _J. ACM_ 19, 2 (April 1972), 248–264. https://doi.org/10.1145/321694.321699

- [32] Babak Falsafi and Thomas F. Wenisch. 2014. _A Primer on Hardware Prefetching_ . Springer Cham, Cham, Switzerland. https://doi.org/10.1007/978-3-031-01743-8

- [33] Andrei Frumusanu. 2020. _Apple Announces The Apple Silicon M1: Ditching x86 - What to Expect, Based on A14_ . Anandtech. https://www.anandtech.com/show/ 16226/apple-silicon-m1-a14-deep-dive/2

- [34] Andrei Frumusanu. 2021. _The Snapdragon 888 vs The Exynos 2100: Cortex-X1 & 5nm - Who Does It Better?_ AnandTech. https://www.anandtech.com/show/ 16463/snapdragon-888-vs-exynos-2100-galaxy-s21-ultra/3

- [35] Ilya Ganusov and Martin Burtscher. 2006. Efficient Emulation of Hardware Prefetchers via Event-Driven Helper Threading. In _Proceedings of the 15th International Conference on Parallel Architectures and Compilation Techniques_ (Seattle, Washington, USA) _(PACT ’06)_ . Association for Computing Machinery, New York, NY, USA, 144–153. https://doi.org/10.1145/1152154.1152178

- [36] Saurabh Gupta, Niranjan Soundararajan, Ragavendra Natarajan, and Sreenivas Subramoney. 2020. Opportunistic Early Pipeline Re-Steering for DataDependent Branches. In _Proceedings of the ACM International Conference on Parallel Architectures and Compilation Techniques_ (Virtual Event, GA, USA) _(PACT ’20)_ . Association for Computing Machinery, New York, NY, USA, 305–316. https://doi.org/10.1145/3410463.3414628

- [37] Tae Jun Ham, Juan L. Aragón, and Margaret Martonosi. 2015. DeSC: Decoupled Supply-compute Communication Management for Heterogeneous Architectures. In _Proceedings of the 48th International Symposium on Microarchitecture_ (Waikiki, Hawaii) _(MICRO-48)_ . Association for Computing Machinery, New

York, NY, USA, 191–203. https://doi.org/10.1145/2830772.2830800

- [38] Milad Hashemi, Khubaib, Eiman Ebrahimi, Onur Mutlu, and Yale N. Patt. 2016. Accelerating Dependent Cache Misses with an Enhanced Memory Controller. In _Proceedings of the 43rd International Symposium on Computer Architecture_ (Seoul, Republic of Korea) _(ISCA ’16)_ . IEEE Computer Society, Los Alamitos, CA, USA, 444–455. https://doi.org/10.1109/ISCA.2016.46

- [39] Milad Hashemi, Onur Mutlu, and Yale N. Patt. 2016. Continuous Runahead: Transparent Hardware Acceleration for Memory Intensive Workloads. In _The 49th Annual IEEE/ACM International Symposium on Microarchitecture_ (Taipei, Taiwan) _(MICRO-49)_ . IEEE Computer Society, Los Alamitos, CA, USA, Article 61, 12 pages. https://doi.org/10.1109/MICRO.2016.7783764

- [40] Milad Hashemi and Yale N. Patt. 2015. Filtered Runahead Execution with a Runahead Buffer. In _Proceedings of the 48th International Symposium on Microarchitecture_ (Waikiki, Hawaii) _(MICRO-48)_ . Association for Computing Machinery, New York, NY, USA, 358–369. https://doi.org/10.1145/2830772.2830812

- [41] Chen-Han Ho, Sung Jin Kim, and Karthikeyan Sankaralingam. 2015. Efficient Execution of Memory Access Phases Using Dataflow Specialization. In _Proceedings of the 42nd Annual International Symposium on Computer Architecture_ (Portland, Oregon) _(ISCA ’15)_ . Association for Computing Machinery, New York, NY, USA, 118–130. https://doi.org/10.1145/2749469.2750390

- [42] Akanksha Jain and Calvin Lin. 2013. Linearizing Irregular Memory Accesses for Improved Correlated Prefetching. In _Proceedings of the 46th Annual IEEE/ACM International Symposium on Microarchitecture_ (Davis, California) _(MICRO-46)_ . Association for Computing Machinery, New York, NY, USA, 247–259. https: //doi.org/10.1145/2540708.2540730

- [43] Doug Joseph and Dirk Grunwald. 1997. Prefetching Using Markov Predictors. In _Proceedings of the 24th Annual International Symposium on Computer Architecture_ (Denver, Colorado, USA) _(ISCA ’97)_ . Association for Computing Machinery, New York, NY, USA, 252–263. https://doi.org/10.1145/264107.264207

- [44] Changhee Jung, Daeseob Lim, Jaejin Lee, and Yan Solihin. 2006. Helper Thread Prefetching for Loosely-Coupled Multiprocessor Systems. In _Proceedings of the 20th International Conference on Parallel and Distributed Processing_ (Rhodes Island, Greece) _(IPDPS’06)_ . IEEE Computer Society, Los Alamitos, CA, USA, 10 pp.–. https://doi.org/10.1109/IPDPS.2006.1639375

- [45] Dongkeun Kim and Donald Yeung. 2002. Design and Evaluation of Compiler Algorithms for Pre-execution. In _Proceedings of the 10th International Conference on Architectural Support for Programming Languages and Operating Systems_ (San Jose, California) _(ASPLOS X)_ . Association for Computing Machinery, New York, NY, USA, 159–170. https://doi.org/10.1145/605397.605415

- [46] Jinchun Kim, Seth H. Pugsley, Paul V. Gratz, A. L. Narasimha Reddy, Chris Wilkerson, and Zeshan Chishti. 2016. Path Confidence Based Lookahead Prefetching. In _The 49th Annual IEEE/ACM International Symposium on Microarchitecture_ (Taipei, Taiwan) _(MICRO-49)_ . IEEE Computer Society, Los Alamitos, CA, USA, Article 60, 12 pages. https://doi.org/10.1109/MICRO.2016.7783763

- [47] Onur Kocberber, Babak Falsafi, and Boris Grot. 2015. Asynchronous Memory Access Chaining. _Proc. VLDB Endow._ 9, 4 (dec 2015), 252–263. https://doi.org/ 10.14778/2856318.2856321

- [48] Onur Kocberber, Boris Grot, Javier Picorel, Babak Falsafi, Kevin Lim, and Parthasarathy Ranganathan. 2013. Meet the Walkers: Accelerating Index Traversals for In-memory Databases. In _Proceedings of the 46th Annual IEEE/ACM International Symposium on Microarchitecture_ (Davis, California) _(MICRO-46)_ . Association for Computing Machinery, New York, NY, USA, 468–479. https: //doi.org/10.1145/2540708.2540748

- [49] Nicholas Kohout, Seungryul Choi, Dongkeun Kim, and Donald Yeung. 2001. Multi-Chain Prefetching: Effective Exploitation of Inter-Chain Memory Parallelism for Pointer-Chasing Codes. In _Proceedings of the 2001 International Conference on Parallel Architectures and Compilation Techniques (PACT ’01)_ . IEEE Computer Society, Los Alamitos, CA, USA, 268–279. https://doi.org/10. 1109/PACT.2001.953307

- [50] Snehasish Kumar, Arrvindh Shriraman, Vijayalakshmi Srinivasan, Dan Lin, and Jordon Phillips. 2014. SQRL: Hardware Accelerator for Collecting Software Data Structures. In _Proceedings of the 23rd International Conference on Parallel Architectures and Compilation_ (Edmonton, AB, Canada) _(PACT ’14)_ . Association for Computing Machinery, New York, NY, USA, 475–476. https://doi.org/10. 1145/2628071.2628118

- [51] Snehasish Kumar, Naveen Vedula, Arrvindh Shriraman, and Vijayalakshmi Srinivasan. 2015. DASX: Hardware Accelerator for Software Data Structures. In _Proceedings of the 29th ACM on International Conference on Supercomputing_ (Newport Beach, California, USA) _(ICS ’15)_ . Association for Computing Machinery, New York, NY, USA, 361–372. https://doi.org/10.1145/2751205.2751231

- [52] Samuel Larsen and Saman Amarasinghe. 2000. Exploiting Superword Level Parallelism with Multimedia Instruction Sets. In _Proceedings of the ACM SIGPLAN 2000 Conference on Programming Language Design and Implementation_ (Vancouver, British Columbia, Canada) _(PLDI ’00)_ . Association for Computing Machinery, New York, NY, USA, 145–156. https://doi.org/10.1145/349299.349320

- [53] Eric Lau, Jason E. Miller, Inseok Choi, Donald Yeung, Saman Amarasinghe, and Anant Agarwal. 2011. Multicore Performance Optimization Using Partner Cores. In _3rd USENIX Workshop on Hot Topics in Parallelism (HotPar 11)_ . USENIX

Association, Berkeley, CA, 1–6. https://www.usenix.org/conference/hotpar11/ multicore-performance-optimization-using-partner-cores

- [54] Erik Lindholm, John Nickolls, Stuart Oberman, and John Montrym. 2008. NVIDIA Tesla: A Unified Graphics and Computing Architecture. _IEEE Micro_ 28, 2 (March 2008), 39–55. https://doi.org/10.1109/MM.2008.31

- [55] Jun Liu, Yuanrui Zhang, Ohyoung Jang, Wei Ding, and Mahmut Kandemir. 2012. A Compiler Framework for Extracting Superword Level Parallelism. In _Proceedings of the 33rd ACM SIGPLAN Conference on Programming Language Design and Implementation_ (Beijing, China) _(PLDI ’12)_ . Association for Computing Machinery, New York, NY, USA, 347–358. https://doi.org/10.1145/2254064.2254106

- [56] Elliot Lockerman, Axel Feldmann, Mohammad Bakhshalipour, Alexandru Stanescu, Shashwat Gupta, Daniel Sanchez, and Nathan Beckmann. 2020. Livia: Data-Centric Computing Throughout the Memory Hierarchy. In _Proceedings of the Twenty-Fifth International Conference on Architectural Support for Programming Languages and Operating Systems_ (Lausanne, Switzerland) _(ASPLOS ’20)_ . Association for Computing Machinery, New York, NY, USA, 417–433. https://doi.org/10.1145/3373376.3378497

- [57] Saeed Maleki, Yaoqing Gao, Maria J. Garzarán, Tommy Wong, and David A. Padua. 2011. An Evaluation of Vectorizing Compilers. In _Proceedings of the 2011 International Conference on Parallel Architectures and Compilation Techniques (PACT ’11)_ . IEEE Computer Society, Los Alamitos, CA, USA, 372–382. https: //doi.org/10.1109/PACT.2011.68

- [58] Pierre Michaud. 2016. Best-offset hardware prefetching. In _2016 IEEE International Symposium on High Performance Computer Architecture (HPCA)_ . IEEE Computer Society, Los Alamitos, CA, USA, 469–480. https://doi.org/10.1109/ HPCA.2016.7446087

- [59] Sparsh Mittal. 2016. A Survey of Recent Prefetching Techniques for Processor Caches. _ACM Comput. Surv._ 49, 2, Article 35 (aug 2016), 35 pages. https: //doi.org/10.1145/2907071

- [60] Andreas Moshovos, Dionisios N. Pnevmatikatos, and Amirali Baniasadi. 2001. Slice-Processors: An Implementation of Operation-Based Prediction. In _Proceedings of the 15th International Conference on Supercomputing_ (Sorrento, Italy) _(ICS ’01)_ . Association for Computing Machinery, New York, NY, USA, 321–334. https://doi.org/10.1145/377792.377856

- [61] Todd Carl Mowry. 1995. _Tolerating Latency through Software-Controlled Data Prefetching_ . Ph. D. Dissertation. Stanford University, Computer Systems Laboratory, Stanford, CA, USA. UMI Order No. GAX94-29983.

- [62] Onur Mutlu, Hyesoon Kim, and Yale N. Patt. 2005. Techniques for Efficient Processing in Runahead Execution Engines. In _Proceedings of the 32nd Annual International Symposium on Computer Architecture (ISCA ’05)_ . IEEE Computer Society, Los Alamitos, CA, USA, 370–381. https://doi.org/10.1109/ISCA.2005.49

- [63] Onur Mutlu, Hyesoon Kim, and Yale N. Patt. 2006. Address-Value Delta (AVD) Prediction: A Hardware Technique for Efficiently Parallelizing Dependent Cache Misses. _IEEE Trans. Comput._ 55, 12 (Dec 2006), 1491–1508. https://doi.org/10. 1109/TC.2006.191

- [64] Onur Mutlu, Hyesoon Kim, and Yale N. Patt. 2006. Efficient Runahead Execution: Power-Efficient Memory Latency Tolerance. _IEEE Micro_ 26, 1 (Jan 2006), 10–20. https://doi.org/10.1109/MM.2006.10

- [65] Onur Mutlu, Hyesoon Kim, Jared Stark, and Yale N. Patt. 2005. On Reusing the Results of Pre-Executed Instructions in a Runahead Execution Processor. _IEEE Computer Architecture Letters_ 4, 1 (Jan 2005), 2–2. https://doi.org/10.1109/LCA.2005.1

- [66] Onur Mutlu, Jared Stark, Chris Wilkerson, and Yale N. Patt. 2003. Runahead execution: an alternative to very large instruction windows for out-of-order processors. In _The Ninth International Symposium on High-Performance Computer Architecture, 2003. HPCA-9 2003. Proceedings._ IEEE Computer Society, Los Alamitos, CA, USA, 129–140. https://doi.org/10.1109/HPCA.2003.1183532

- [67] Ajeya Naithani, Sam Ainsworth, Timothy M. Jones, and Lieven Eeckhout. 2021. Vector Runahead. In _Proceedings of the 48th Annual International Symposium on Computer Architecture_ (Virtual Event, Spain) _(ISCA ’21)_ . IEEE Computer Society, Los Alamitos, CA, USA, 195–208. https://doi.org/10.1109/ISCA52012.2021.00024

- [68] Ajeya Naithani, Sam Ainsworth, Timothy M. Jones, and Lieven Eeckhout. 2022. Vector Runahead for Indirect Memory Accesses. _IEEE Micro_ 42, 4 (jul 2022), 116–123. https://doi.org/10.1109/MM.2022.3163132

- [69] Ajeya Naithani, Josué Feliu, Almutaz Adileh, and Lieven Eeckhout. 2019. Precise Runahead Execution. _IEEE Computer Architecture Letters_ 18, 1 (Jan 2019), 71–74. https://doi.org/10.1109/LCA.2019.2910518

- [70] Ajeya Naithani, Josué Feliu, Almutaz Adileh, and Lieven Eeckhout. 2020. Precise Runahead Execution. In _2020 IEEE International Symposium on High Performance Computer Architecture (HPCA)_ . IEEE Computer Society, Los Alamitos, CA, USA, 397–410. https://doi.org/10.1109/HPCA47549.2020.00040

- [71] Agustín Navarro-Torres, Biswabandan Panda, Jesús Alastruey-Benedé, Pablo Ibáñez, Víctor Viñals-Yúfera, and Alberto Ros. 2022. Berti: an Accurate LocalDelta Data Prefetcher. In _2022 55th IEEE/ACM International Symposium on Microarchitecture (MICRO-55)_ . IEEE Computer Society, Los Alamitos, CA, USA, 975–991. https://doi.org/10.1109/MICRO56248.2022.00072

- [72] Kyle J. Nesbit and James E. Smith. 2004. Data Cache Prefetching Using a Global History Buffer. In _Proceedings of the 10th International Symposium on High_

- _Performance Computer Architecture (HPCA ’04)_ . IEEE Computer Society, Los Alamitos, CA, USA, 96. https://doi.org/10.1109/HPCA.2004.10030

- [73] Quan M. Nguyen and Daniel Sanchez. 2020. Pipette: Improving Core Utilization on Irregular Applications through Intra-Core Pipeline Parallelism. In _2020 53rd Annual IEEE/ACM International Symposium on Microarchitecture (MICRO)_ . IEEE Computer Society, Los Alamitos, CA, USA, 596–608. https://doi.org/10.1109/ MICRO50266.2020.00056

- [74] Dorit Nuzman, Ira Rosen, and Ayal Zaks. 2006. Auto-Vectorization of Interleaved Data for SIMD. In _Proceedings of the 27th ACM SIGPLAN Conference on Programming Language Design and Implementation_ (Ottawa, Ontario, Canada)

- _(PLDI ’06)_ . Association for Computing Machinery, New York, NY, USA, 132–143. https://doi.org/10.1145/1133981.1133997

- [75] Samuel Pakalapati and Biswabandan Panda. 2020. Bouquet of Instruction Pointers: Instruction Pointer Classifier-based Spatial Hardware Prefetching. In _2020 ACM/IEEE 47th Annual International Symposium on Computer Architecture (ISCA)_ . IEEE Computer Society, Los Alamitos, CA, USA, 118–131. https://doi.org/10.1109/ISCA45697.2020.00021

- [76] Vasileios Porpodas and Timothy M. Jones. 2015. Throttling Automatic Vectorization: When Less is More. In _Proceedings of the 2015 International Conference on Parallel Architecture and Compilation (PACT) (PACT ’15)_ . IEEE Computer Society, Los Alamitos, CA, USA, 432–444. https://doi.org/10.1109/PACT.2015.32

- [77] Vasileios Porpodas, Alberto Magni, and Timothy M. Jones. 2015. PSLP: Padded SLP Automatic Vectorization. In _Proceedings of the 13th Annual IEEE/ACM International Symposium on Code Generation and Optimization_ (San Francisco, California) _(CGO ’15)_ . IEEE Computer Society, Los Alamitos, CA, USA, 190–201. https://doi.org/10.1109/CGO.2015.7054199

- [78] Stephen Pruett and Yale Patt. 2021. Branch Runahead: An Alternative to Branch Prediction for Impossible to Predict Branches. In _MICRO-54: 54th Annual IEEE/ACM International Symposium on Microarchitecture_ (Virtual Event, Greece) _(MICRO ’21)_ . Association for Computing Machinery, New York, NY, USA, 804–815. https://doi.org/10.1145/3466752.3480053

- [79] Tanausú Ramírez, Alex Pajuelo, Oliverio Jesus Santana, Onur Mutlu, and Mateo Valero. 2010. Efficient Runahead Threads. In _Proceedings of the 19th International Conference on Parallel Architectures and Compilation Techniques_ (Vienna, Austria) _(PACT ’10)_ . Association for Computing Machinery, New York, NY, USA, 443–452. https://doi.org/10.1145/1854273.1854328

- [80] Tanausú Ramírez, Alex Pajuelo, Oliverio Jesus Santana, and Mateo Valero. 2008. Runahead Threads to improve SMT performance. In _2008 IEEE 14th International Symposium on High Performance Computer Architecture_ . IEEE Computer Society, Los Alamitos, CA, USA, 149–158. https://doi.org/10.1109/HPCA.2008.4658635

- [81] Ram Rangan, Neil Vachharajani, Manish Vachharajani, and David I. August. 2004. Decoupled Software Pipelining with the Synchronization Array. In _Proceedings of the 13th International Conference on Parallel Architectures and Compilation Techniques (PACT ’04)_ . IEEE Computer Society, Los Alamitos, CA, USA, 177–188. https://doi.org/10.1109/PACT.2004.1342552

- [82] Amir Roth, Andreas Moshovos, and Gurindar S. Sohi. 1998. Dependence Based Prefetching for Linked Data Structures. In _Proceedings of the Eighth International Conference on Architectural Support for Programming Languages and Operating Systems_ (San Jose, California, USA) _(ASPLOS VIII)_ . Association for Computing Machinery, New York, NY, USA, 115–126. https://doi.org/10.1145/291069.291034

- [83] André Seznec. 2016. TAGE-SC-L Branch Predictors Again. In _5th JILP Workshop on Computer Architecture Competitions (JWAC-5): Championship Branch Prediction (CBP-5)_ (Seoul, South Korea). INRIA HAL, rennes France, 1–4. https: //inria.hal.science/hal-01354253

- [84] Manjunath Shevgoor, Sahil Koladiya, Rajeev Balasubramonian, Chris Wilkerson, Seth H. Pugsley, and Zeshan Chishti. 2015. Efficiently Prefetching Complex Address Patterns. In _Proceedings of the 48th International Symposium on Microarchitecture_ (Waikiki, Hawaii) _(MICRO-48)_ . Association for Computing Machinery, New York, NY, USA, 141–152. https://doi.org/10.1145/2830772.2830793

- [85] Zhan Shi, Akanksha Jain, Kevin Swersky, Milad Hashemi, Parthasarathy Ranganathan, and Calvin Lin. 2021. A Hierarchical Neural Model of Data Prefetching. In _Proceedings of the 26th ACM International Conference on Architectural Support for Programming Languages and Operating Systems_ (Virtual, USA) _(ASPLOS ’21)_ . Association for Computing Machinery, New York, NY, USA, 861–873. https://doi.org/10.1145/3445814.3446752

- [86] Peng Sun, Giacomo Gabrielli, and Timothy M. Jones. 2021. Speculative Vectorisation with Selective Replay. In _Proceedings of the 48th Annual International Symposium on Computer Architecture_ (Virtual Event, Spain) _(ISCA ’21)_ . IEEE Computer Society, Los Alamitos, CA, USA, 223–236. https://doi.org/10.1109/ ISCA52012.2021.00026

- [87] Hikaru Takayashiki, Masayuki Sato, Kazuhiko Komatsu, and Hiroaki Kobayashi. 2019. A Hardware Prefetching Mechanism for Vector Gather Instructions. In _2019 IEEE/ACM 9th Workshop on Irregular Applications: Architectures and Algorithms (IA3)_ . IEEE Computer Society, Los Alamitos, CA, USA, 59–66. https: //doi.org/10.1109/IA349570.2019.00015

- [88] Nishil Talati, Kyle May, Armand Behroozi, Yichen Yang, Kuba Kaszyk, Christos Vasiladiotis, Tarunesh Verma, Lu Li, Brandon Nguyen, Jiawen Sun, John Magnus Morton, Agreen Ahmadi, Todd Austin, Michael O’Boyle, Scott Mahlke, Trevor

Mudge, and Ronald Dreslinski. 2021. Prodigy: Improving the Memory Latency of Data-Indirect Irregular Workloads Using Hardware-Software Co-Design, In 2021 IEEE International Symposium on High-Performance Computer Architecture. _Proceedings - International Symposium on High-Performance Computer Architecture_ 2021-February, 654–667. https://doi.org/10.1109/HPCA51647.2021.00061

- [89] Sam Ainsworth Timothy and M. Jones. 2017. Software prefetching for indirect memory accesses. In _CGO 2017 - Proceedings of the 2017 International Symposium on Code Generation and Optimization_ . IEEE Computer Society, Los Alamitos, CA, USA, 305–317. https://doi.org/10.1109/CGO.2017.7863749

- [90] Kim-Anh Tran, Trevor E. Carlson, Konstantinos Koukos, Magnus Själander, Vasileios Spiliopoulos, Stefanos Kaxiras, and Alexandra Jimborean. 2017. Clairvoyance: Look-Ahead Compile-Time Scheduling. In _Proceedings of the 2017 International Symposium on Code Generation and Optimization_ (Austin, USA) _(CGO ’17)_ . IEEE Computer Society, Los Alamitos, CA, USA, 171–184. https: //doi.org/10.1109/CGO.2017.7863738

- [91] Dean M. Tullsen, Susan J. Eggers, and Henry M. Levy. 1995. Simultaneous Multithreading: Maximizing on-Chip Parallelism. In _Proceedings of the 22nd Annual International Symposium on Computer Architecture_ (S. Margherita Ligure, Italy) _(ISCA ’95)_ . Association for Computing Machinery, New York, NY, USA, 392–403. https://doi.org/10.1145/223982.224449

- [92] Neil Vachharajani, Ram Rangan, Easwaran Raman, Matthew J. Bridges, Guilherme Ottoni, and David I. August. 2007. Speculative Decoupled Software Pipelining. In _2007 16th International Conference on Parallel Architectures and Compilation Techniques_ . IEEE Computer Society, Los Alamitos, CA, USA, 49–59. https://doi.org/10.1109/PACT.2007.66

- [93] Perry H. Wang, Jamison D. Collins, Hong Wang, Dongkeun Kim, Bill Greene, Kai-Ming Chan, Aamir B. Yunus, Terry Sych, Stephen F. Moore, and John P. Shen. 2004. Helper Threads via Virtual Multithreading. _IEEE Micro_ 24, 6 (nov 2004), 74–82. https://doi.org/10.1109/MM.2004.75

- [94] Zhenlin Wang, Doug Burger, Kathryn S. McKinley, Steven K. Reinhardt, and Charles C. Weems. 2003. Guided Region Prefetching: A Cooperative Hardware/Software Approach. In _Proceedings of the 30th Annual International Symposium on Computer Architecture_ (San Diego, California) _(ISCA ’03)_ . Association for Computing Machinery, New York, NY, USA, 388–398. https:

//doi.org/10.1145/859618.859663

- [95] Hao Wu, Krishnendra Nathella, Joseph Pusdesris, Dam Sunwoo, Akanksha Jain, and Calvin Lin. 2019. Temporal Prefetching Without the Off-Chip Metadata. In _Proceedings of the 52nd Annual IEEE/ACM International Symposium on Microarchitecture_ (Columbus, OH, USA) _(MICRO ’52)_ . Association for Computing Machinery, New York, NY, USA, 996–1008. https://doi.org/10.1145/3352460. 3358300

- [96] Hao Wu, Krishnendra Nathella, Dam Sunwoo, Akanksha Jain, and Calvin Lin. 2019. Efficient Metadata Management for Irregular Data Prefetching. In _Proceedings of the 46th International Symposium on Computer Architecture_ (Phoenix, Arizona) _(ISCA ’19)_ . Association for Computing Machinery, New York, NY, USA, 449–461. https://doi.org/10.1145/3307650.3322225

- [97] Chia-Lin Yang and Alvin R. Lebeck. 2002. A Programmable Memory Hierarchy for Prefetching Linked Data Structures. In _Proceedings of the 4th International Symposium on High Performance Computing (ISHPC ’02)_ . Springer-Verlag, Berlin, Heidelberg, 160–174. https://doi.org/10.1007/3-540-47847-7_15

- [98] Xiangyao Yu, Christopher J. Hughes, Nadathur Satish, and Srinivas Devadas. 2015. IMP: Indirect Memory Prefetcher. In _Proceedings of the 48th International Symposium on Microarchitecture_ (Waikiki, Hawaii) _(MICRO-48)_ . Association for Computing Machinery, New York, NY, USA, 178–190. https://doi.org/10.1145/ 2830772.2830807

- [99] Chao Zhang, Yuan Zeng, John Shalf, and Xiaochen Guo. 2020. RnR: A SoftwareAssisted Record-and-Replay Hardware Prefetcher. In _2020 53rd Annual IEEE/ACM International Symposium on Microarchitecture (MICRO)_ . IEEE Computer Society, Los Alamitos, CA, USA, 609–621. https://doi.org/10.1109/MICRO50266.2020. 00057

- [100] Dan Zhang, Xiaoyu Ma, Michael Thomson, and Derek Chiou. 2018. Minnow: Lightweight Offload Engines for Worklist Management and Worklist-Directed Prefetching. In _Proceedings of the Twenty-Third International Conference on Architectural Support for Programming Languages and Operating Systems_ (Williamsburg, VA, USA) _(ASPLOS ’18)_ . Association for Computing Machinery, New York, NY, USA, 593–607. https://doi.org/10.1145/3173162.3173197




## Runahead、VR和DVR对比

|维度|传统Runahead/PRE|Vector Runahead（VR）|Decoupled Vector Runahead（DVR）|
|---|---|---|---|
|启动条件|ROB满且头部load阻塞|ROB满|检测到合适的stride load和依赖链|
|执行位置|主OoO流水线进入runahead模式|主OoO流水线进入VR模式|独立的轻量级顺序子线程|
|主线程能否同时前进|不能|通常不能，delayed termination还会阻塞commit|可以|
|指令生成|依赖主前端获取未来指令|自动生成并向量化load chain|自动生成，使用独立子线程执行|
|并行方式|有限的标量MLP|同一循环内多迭代向量化|SIMT向量化，可覆盖多个循环实例|
|多级间接访问|覆盖有限|可以沿多级间接链预取|可以，并且更主动、更持续|
|循环边界|不显式识别|默认尽量扩大向量化|Discovery Mode动态推断|
|短内循环|MLP有限|容易越界预取或并行度不足|Nested Mode组合多个外循环实例|
|分支发散|沿推测控制流执行|跟随第一个lane，其他发散lane失效|GPU式发散与再汇合|
|对ROB大小的敏感性|ROB越大，越难触发|ROB越大，收益通常下降|与ROB满解耦，大ROB下仍有效|
|硬件开销|Checkpoint及runahead支持|向量寄存器与VR控制|额外约1139字节控制状态|
|论文平均性能|较低|约为OoO的1.2×|约为OoO的2.4×、VR的2×|

一句话概括演进过程：

```
Runahead：主核停顿后，标量地寻找未来miss
    ↓
VR：主核停顿后，向量化执行未来访存链
    ↓
DVR：独立子线程主动、动态、支持分支发散地执行向量访存链
```

DVR最核心的变化不是单纯“向量更宽”，而是：

> **将向量前瞻执行从ROB停顿驱动的临时模式，变成与主线程并行运行、自适应控制的预取子线程。**