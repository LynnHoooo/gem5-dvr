# DVR 论文配置与当前复现代码对照

> 更新时间：2026-08-08  
> 适用代码：`code/gem5-runahead-dev-pre/`  
> 项目定位：RISC-V/gem5 上的 DVR 机制原型，不是论文 x86/Sniper 结果的逐项复刻。

## 1. 结论摘要

当前实现已经覆盖 DVR 的主要机制框架：stride detection、Discovery、VTT/FLR、loop-bound inference、helper、向量 lane、predicate/reconvergence、NDM 和真实 cache timing 路径。

但它与论文仍有三类差异：

1. **ISA 和模拟器不同**：论文使用 x86/AVX-512 + Sniper，当前使用 RISC-V + gem5。
2. **硬件资源表示更宽松**：部分表项、快照、stack 和 recorder 使用完整软件字段，不能直接用论文的字节数宣称硬件开销相同。
3. **部分语义是适配或 fallback**：当前代码支持 RISC-V 指令子集；对 unsupported 链保留 affine fallback，因此需要单独报告覆盖范围，不能称为完全算法复现。

因此，当前复现适合表述为：

> **论文机制的 RISC-V/gem5 适配型复现（mechanism-level reproduction）**，并通过消融和事件统计验证各模块的作用；性能数值不能直接与论文 Figure 7/8 的绝对结果比较。

## 2. 硬件结构对照

| 硬件结构 | 论文配置 | 当前实现 | 差异与影响 |
|---|---|---|---|
| Stride Detector / RPT | 32 entries，论文估算 460 B | `dvrRPTEntries=32`；每项使用 64-bit PC、地址、stride 和 age | entry 数一致，但字段未按论文压缩，实际面积不能按 460 B 计算 |
| 最大向量宽度 | 128 lanes | 128 lanes；16 个 512-bit vector copy × 8 lanes | 逻辑 lane 数一致，后端由 RISC-V/gem5 软件结构模拟 |
| VRAT | 16 entries，288 B | 32 个架构寄存器、16 个 vector copies；256 个 scalar + 128 个 vector 物理资源 | 适配 RISC-V 32 个 GPR；表项和物理资源组织不同 |
| VIR | 86 B | `VIRCopies=16`、每 copy 8 lanes；另有 `VIRCapacity=8` | 功能结构相近，但字段和容量不是论文的硬件编码 |
| Front-end buffer | 8 uops，64 B | `FrontEndBufferCapacity=8`；recorder 实际允许 `MaxUops=256` | 前端容量参数一致，但 recorder 上限更大，可能影响 overflow/终止行为 |
| Reconvergence stack | 8 entries，176 B，共享 mask/PC | 深度为 8，但当前按 lane 保存，最多 128×8 个 stack frame | 逻辑上支持 reconvergence，硬件存储量显著大于论文 |
| FLR | 6 B | 64-bit `Addr` | 语义对应；编码宽度更宽 |
| LCR | 2 B | 64-bit branch/reconvergence PC 及寄存器状态 | 当前保存的信息更多，不能按 2 B 估算 |
| SBB | 1 bit | 多个 `bool` 和路径状态 | 功能对应，未压缩为论文单 bit |
| Loop-bound detector | 48 B；两个 checkpoint 和 branch 信息 | 两个 `32×64-bit` 寄存器快照 | 能支持 RISC-V 32 GPR，但远大于论文的压缩实现 |
| Taint tracker | 16 bits | 32-bit GPR taint bitmap + 64-bit FLR PC | 论文 x86 16 个整数寄存器的适配扩展 |
| NDM 控制状态 | IR/ILR/LCR、branch inversion、outer invocation 收集 | `MaxDepth=2`、`MaxOuterInvocations=16` | 已有对应控制状态，但深度/收集上限属于当前原型参数 |
| Helper 资源 | 论文采用共享资源模型 | issue width 4、issue queue 8、load queue 16、prefetch queue 256 | 当前加入显式资源模型；与论文的抽象资源模型不完全同构 |

### 硬件层面的判定

- **结构存在性**：大部分模块已经存在。
- **功能容量**：128 lanes、16 vector copies、8-entry reconvergence stack 等主要数量已对齐。
- **面积/位宽等价性**：尚未对齐。当前实现应报告“功能容量”和“实际软件字段大小”两套数据，不应直接复用论文的 1139 B 总开销。

## 3. 逻辑实现对照

| 论文逻辑 | 论文行为 | 当前代码对应 | 当前复现判断 |
|---|---|---|---|
| Stride detection | RPT 识别稳定 stride，选择候选 trigger | `DVRStrideDetector`，dispatch/load 地址观察 | 机制已实现；RPT 字段编码不同 |
| Discovery | 沿主线程一次迭代跟踪，识别更内层 stride、dependent load 和 loop bound | `DVRDiscoveryController`、VTT、FLR、loop-bound detector | 主流程已实现；需用事件统计确认每条链的实际覆盖 |
| VTT | taint 从 trigger load 结果传播到后续操作 | `DVRVectorTaintTracker`，RISC-V x0–x31 | ISA 适配；从 16-bit 扩展为 32-bit |
| FLR | 记录最后一个地址依赖 load | 地址源 tainted 的 load 更新 FLR | 语义基本对应 |
| Loop bound | backward branch、LCR/SBB 和两次快照推断 bound/increment/remaining iterations | `DVRLoopBoundDetector` | 已实现 RISC-V 分支比较和 signed arithmetic；需注意异常控制流/指令子集边界 |
| Helper spawn | Discovery 完成后创建独立 vector-runahead 子线程 | `CPU::instDone()` 及 helper 生命周期 | 已有独立 helper 前端和状态 |
| VRAT/VIR | 映射 scalar/vector registers，按 lane 执行模板 | `DVRVectorRenameTable`、`DVRVectorInstructionRegister` | 已有真实状态和统计；对 unsupported 指令保留 fallback |
| Branch divergence | active mask + reconvergence stack，按路径继续执行 | `dvr_predicate.*`、lane predicate tracker | 已有逐 lane mask/reconvergence 路径；资源组织与论文不同 |
| Source prefetch | 产生未来 striding addresses | `launchDVRStridePrefetches()` | 进入 gem5 timing memory system |
| Dependent replay | source response 返回后继续 dependent chain | `replayDVRSource()`、`completeDVRPrefetch()` | 有真实 response 驱动的 replay 路径 |
| NDM / Nested DVR | 短 inner loop 时跳出 inner loop，向量化 outer invocation，再收集最多 128 个 inner targets | `DVRNestedController`、`DVRNestedDiscoveryMode` | 已有 IR/ILR/LCR、branch inversion、outer invocation gate；`MaxDepth=2` 是原型限制 |
| 终止条件 | 到达 FLR、下一 stride PC、或 200-uop 超时等 | normal completion、branch early exit、external target、timeout 等统计 | 已显式分类；需避免把 fallback completion 当作论文语义完整执行 |

### 逻辑层面的主要未等价点

1. **指令覆盖范围**：论文面向 x86/AVX-512；当前只支持已实现的 RISC-V 指令语义。
2. **fallback**：unsupported dependent chain 可走 affine fallback，这能帮助机制继续运行，但不等于论文中的真实动态执行。
3. **状态粒度**：当前 recorder、lane stack、寄存器快照保存的信息更多，可能降低资源压力并改变终止概率。
4. **NDM 深度**：当前 `MaxDepth=2`，不应直接宣称覆盖任意深度的 nested loop。

## 4. 仿真环境与实验配置对照

| 维度 | 论文 | 当前复现 | 含义 |
|---|---|---|---|
| ISA | x86-64 | RISC-V RV64 | 指令、寄存器数、分支编码和向量后端不同 |
| 模拟器 | Sniper 6.0，详细周期级 x86 core model | gem5 O3，RISC-V 机制改造 | 两者的 pipeline、cache、统计和 timing model 不同 |
| 主核 | 4 GHz、5-wide OoO、350-entry ROB | Table 1 风格 gem5 O3 配置 | 参数可近似，但不能视为同一微架构 |
| L1D | 32 KB、8-way、4 cycles、24 MSHRs、16-stream stride prefetcher | 由 `configs/dvr/table1_se.py` 配置 | 需要以实际 gem5 config dump 为准 |
| L2/L3 | 私有 256 KB L2，共享 8 MB L3 | gem5 对应 cache hierarchy | cache timing 和请求仲裁实现不同 |
| 内存 | 50 ns，51.2 GB/s，request-based contention | gem5 memory system | 带宽/延迟参数若未逐项校准，不能直接比较绝对 cycles |
| 工作负载 | 13 个 graph/database/HPC benchmark，ROI 后 500M instructions | 当前已有微基准、GAP/LeAP smoke 和 gem5 SE workload | 目前主要用于机制验证；论文规模和输入覆盖需单独完成 |
| 图输入 | Kron、LiveJournal、Orkut、Twitter、Urand | 当前实验需明确实际输入和规模 | 输入不同会显著改变 inner-loop 长度、MLP 和 cache miss |
| 对比方法 | OoO、PRE、IMP、VR、DVR、Oracle | 当前可做 Baseline、VR-like、Offload、Discovery、Full/Nested DVR 消融 | 需要统一 binary、ROI、cache 参数和统计定义 |
| 主要指标 | normalized IPC、speedup、MLP/MSHR、性能分解 | cycles、IPC、L1D misses、helper、conflict、accuracy、coverage 等 | 当前指标更偏机制/质量分析；与论文图需建立映射 |

## 5. Figure 8 消融映射

论文 Figure 8 的四个阶段在当前代码中应按下表记录：

| Figure 8 阶段 | 论文含义 | 当前实验映射 | 需要控制的变量 |
|---|---|---|---|
| VR | 原始 Vector Runahead | `--dvr-mode=vr` 或对应 VR-like 模式 | 不启用 Offload、Discovery、NDM |
| Offload | 检测到 stride 即启动 vector-runahead helper | `--dvr-mode=offload` | 只增加 helper spawn，不加入 Discovery/NDM |
| +Discovery | 用 Discovery 减少错误路径和 over-fetch | `--dvr-mode=discovery` | 保持 lane、cache、helper 资源一致 |
| +Multiple | Nested Runahead/NDM，跨多个 inner-loop invocation 收集 targets | `--dvr-mode=full` 或 Nested DVR | 只增加 NDM/flattening，其他配置不变 |

当前 Figure 8 复现应优先比较：

- cycles / IPC 或 normalized IPC；
- demand L1D misses；
- helper issued/completed；
- useful/late/invalid prefetch；
- MSHR、带宽和 cache pollution；
- 每阶段触发次数及 NDM 收集到的 outer/inner invocation 数。

## 6. 复现可信度分级

| 层级 | 当前状态 | 可以如何表述 |
|---|---|---|
| 机制框架 | 已具备 | “实现了 DVR 的主要模块和执行路径” |
| 参数数量 | 部分对齐 | “lane 数、RPT entry 数、vector copy 数等关键容量对齐” |
| 硬件面积 | 未严格对齐 | “当前字段实现大于论文压缩格式，面积仅作上界/结构性估计” |
| ISA 语义 | RISC-V 适配 | “在支持的 RV64 指令子集上执行” |
| 论文算法 | 部分对齐 | “实现论文核心算法，并包含显式 fallback 和原型限制” |
| 性能数字 | 不可直接等同 | “用于相对消融和趋势验证，不能直接复现论文绝对 speedup” |
| 论文级完整复现 | 尚未达到 | 需要同等 benchmark/input、统一统计口径和更完整指令覆盖 |

## 7. 后续补齐优先级

1. **先固定实验口径**：确定 baseline、VR、Offload、Discovery、Nested DVR 的开关关系，以及统一 ROI、输入规模和 cache 参数。
2. **再补齐统计映射**：把论文的 normalized IPC、MLP/MSHR、prefetch accuracy/coverage 与当前 gem5 stats 对齐。
3. **单独统计 NDM 附带开销**：区分 NDM outer loads、最终 inner dependent prefetch、主线程 demand miss 和 cache pollution。
4. **补充指令/控制流覆盖报告**：统计 affine fallback、unsupported control flow、timeout、stack overflow 和 recorder overflow。
5. **最后再做论文规模 workload**：在机制 smoke 通过后，逐步扩展到 GAP 输入和 500M-instruction ROI，避免把原型失败与环境问题混在一起。

## 8. 一句话定位

当前代码不是“把论文的 x86 模拟器配置原样搬到 gem5”，而是：

> **在 RISC-V/gem5 O3 平台上，按论文的模块划分实现 DVR/NDM 的可运行机制原型，并用消融、质量和 timing 统计验证其设计逻辑。**

## 9. 最新 Camel dependent-load 修复结果（2026-08-08）

### 9.1 问题定位

Camel 的 dependent load PC 是 `0x103aa`，对应 `*array2[i]`。旧实现中，128 lanes 被错误地当成每次 discovery 都可以重复发射的 source/dependent stream 数量。结果是 dependent target 过度超前：helper 请求虽然生成并完成，但在主线程真正访问前已经被 L1 驱逐。

因此，旧结果中 source miss 已经接近于零，而 dependent demand miss 仍然较高。这不是 128 lanes 本身错误，而是 vector launch 生命周期和 target window 没有建立边界。

### 9.2 当前修复

当前 `launchDVRStridePrefetches()` 使用 launch-level dependent window：

- 每次 vector launch 最多 128 lanes；
- 统计 queued、outstanding、completed dependent target lines；
- 已有 128 条 target 尚未被主线程消费时，抑制新的 helper launch；
- 已经启动的 vector launch 不截断、不丢 lane；
- 不再让 replay lane 长期保持 active，避免长流 helper context 累积。

这恢复了论文中“128 lanes 是一次有界 vector execution”的语义。

### 9.3 Camel full-run 结果

测试命令：

```bash
MAX_KEY=65536 \\
OUT=/home/lynnhoo/dvr-repro/results/camel-dvr-trace-max65536-launchgate \\
DVR_MODE=nested \\
bash scripts/run_camel_dvr_trace.sh
```

关键结果：

| 指标 | 旧结果 | 当前 launch-gate | 变化 |
|---|---:|---:|---:|
| `0x103aa` 主线程 demand miss | 58,428 | **1,593** | **下降 97.3%** |
| dependent prefetch generated | — | 64,432 | — |
| dependent prefetch issued | — | 64,432 | 无丢失 |
| dependent prefetch completed | — | 64,432 | 无丢失 |
| `dvrDependentDemandLate` | — | 0 | 没有 late dependent demand |
| `dvrDependentWindowStalls` | — | 14,955 | 新 launch 被窗口背压 |
| 总 `dcache.demandMisses` | — | 33,627 | 完整 Camel run |

当前结果目录：

```text
/home/lynnhoo/dvr-repro/results/camel-dvr-trace-max65536-launchgate/
```

`pc_pipeline_summary.csv` 中的关键行：

```text
0x103aa,63943,1593,0,0,7552,56880,350,0,0,15743,15743,67,67,15809,64432,0,0
```

这里的第二列和第三列是主线程 `demand_hits`/`demand_misses`。`dvr_dependent_misses` 是 helper 侧的事件统计，不能直接当作主线程 demand miss；评价 cache 效果时应使用 `demand_misses`。

### 9.4 结果解释

当前结果说明：

1. source-to-dependent 数据流已经能生成正确的 dependent target；
2. dependent target 的生成、发射和完成数量一致，launch-gate 没有丢弃已启动 vector 的 lane；
3. 主要残余 miss 从约 2.8 万级降到 1,593，证明根因是 target lead/L1 eviction，而不是 source 地址计算错误；
4. 剩余 1,593 个 miss 仍可能来自 cache line 竞争、请求时序和主线程与 helper 的共享资源竞争，不能宣称 dependent miss 已达到零。

因此，当前 Camel 结果可以支持“DVR dependent prefetch dataflow 在 RISC-V/gem5 上有效”的结论，但仍应在 GAP 五个 workload 上用相同口径完成六组消融后，再给出跨 workload 的论文级结论。

### 9.5 验证状态

当前二进制使用固定的 Python 3.6 ABI 环境编译，并已通过：

- `MAX_KEY=4096` short Camel trace；
- `MAX_KEY=16384` medium Camel trace；
- `MAX_KEY=65536` full Camel trace。

编译命令：

```bash
cd /home/lynnhoo/dvr-repro/source/gem5-dvr/code/gem5-runahead-dev-pre
export PYTHON_CONFIG=/tmp/python36-config
python3 -m SCons build/RISCV/gem5.opt -j32
```
