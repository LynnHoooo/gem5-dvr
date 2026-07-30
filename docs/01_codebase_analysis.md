# DVR 复现：代码库分析与主干选择

> 状态：第一轮静态分析完成  
> 目标：判断两套 gem5 源码各自实现了什么、DVR 应基于哪一套继续开发，以及第一批改动应落在哪里。

## 1. 结论先行

**DVR 的开发主干应选择 `code/gem5-runahead-dev-pre`。**

原因不是它更新，而是它已经完成了对 O3CPU runahead 所需关键路径的侵入式修改：

- 在 `BaseO3CPU` 中加入 PRE 开关与 SST/PRDQ 参数；
- 在 ROB 满且 head load 未完成时进入 PRE；
- 用 SST 只保留 stalling slice；
- 在 decode/rename 阶段丢弃非 slice 指令；
- PRE 指令进入 PRDQ 而不是 ROB；
- 支持 PRE 退出、恢复 rename 状态和提前回收物理寄存器；
- 已提供最小的 RISC-V SE 模式运行脚本。

`code/gem5-triangel-stable` 是 **ISCA 2024 Triangel 时间预取器**的 artifact。它的主要改动位于 cache/prefetch 层，不包含 runahead 子线程、SST、PRDQ 或 O3CPU 的 PRE 状态。它适合作为以下内容的参考，而不是 DVR 主干：

- 较新的 gem5 23.0.0.1 代码和构建方式；
- x86 Full-System、checkpoint 和实验脚本；
- prefetch usefulness、metadata、cache traffic 等统计方法；
- 可复现实验 artifact 的目录组织。

短期内不要把 PRE patch 直接移植到 Triangel 树。两套代码分别基于 gem5 22.0.0.1 和 23.0.0.1，O3CPU 接口变化很大；先在 PRE 树实现 DVR 功能模型，可以显著减少无关的移植工作。

## 2. 两套源码的身份

| 目录 | 上游基础 | 论文/机制 | 主要修改层级 | 对 DVR 的价值 |
| --- | --- | --- | --- | --- |
| `code/gem5-runahead-dev-pre` | gem5 22.0.0.1 | HPCA 2020 Precise Runahead Execution | O3 pipeline、rename、commit、寄存器与 slice tracking | **主干**；提供 runahead 状态与 O3 修改入口 |
| `code/gem5-triangel-stable` | gem5 23.0.0.1 | ISCA 2024 Triangel temporal prefetcher | L2/L3 prefetcher、cache metadata、FS 实验脚本 | 参考；用于实验工程、统计与较新 API 对照 |

两套目录都没有独立 `.git` 元数据，因此不能依赖 `git log` 还原作者修改历史。当前分析依据 README、release notes、文件结构和源码中的机制标记。

## 3. PRE 树已经实现了什么

### 3.1 配置入口

文件：`code/gem5-runahead-dev-pre/src/cpu/o3/BaseO3CPU.py`

已有参数：

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| `enablePRE` | `False` | 启用 Precise Runahead |
| `enablePREBranch` | `False` | 允许特定 branch 进入 stalling slice |
| `enablePREEarlyRecycle` | `False` | PRE 中提前回收物理寄存器 |
| `numPRDQEntries` | 192 | Precise Register Deallocation Queue 容量 |
| `numSSTEntries` | 128 | Stalling Slice Table 容量 |

这些参数证明 PRE 不是一个 cache-side prefetcher，而是 O3CPU 的执行模式。

### 3.2 SST：动态保存 stalling slice

文件：

- `code/gem5-runahead-dev-pre/src/cpu/o3/pre.hh`
- `code/gem5-runahead-dev-pre/src/cpu/o3/pre.cc`

`SST` 是一个以指令 PC 为键的 LRU 表：

- `addInst()` 插入导致 stall 的 load 或其依赖 slice；
- `hasInst()` 判断未来取到的动态指令是否属于 slice；
- 默认 128 项；
- 表项只保存 PC，不保存 DVR 所需的 stride、taint、loop bound 或 lane 状态。

SST 对 DVR 的直接价值有限，因为 DVR 不是“只重放导致 full-ROB stall 的 slice”。但它提供了一个很有价值的范例：如何在 O3 pipeline 外维护动态 PC 集合，并在 decode/rename 中筛选指令。

### 3.3 PRE 的触发条件

文件：`code/gem5-runahead-dev-pre/src/cpu/o3/commit.cc`

当前触发逻辑是：

1. commit 发现 full-window stall；
2. ROB head 是尚未完成的 load；
3. `enablePRE=True` 且当前不在 PRE；
4. 把 head load 加入 SST；
5. 调用 `CPU::enterPRE()`。

这正是 DVR 论文希望移除的限制。DVR 的第一项结构性改动应当是：

- 保留 PRE 触发路径，用作 PRE baseline；
- 新增独立的 DVR controller；
- DVR 由 stride detector + Discovery Mode 触发，不依赖 ROB 满；
- PRE 和 DVR 的状态、统计与配置必须分开，不能把 DVR 伪装成 PRE 的新开关。

### 3.4 decode/rename 中的 slice 筛选

文件：

- `code/gem5-runahead-dev-pre/src/cpu/o3/decode.cc`
- `code/gem5-runahead-dev-pre/src/cpu/o3/rename.cc`

PRE 模式中：

- decode 查询 SST；
- 非 SST 指令被丢弃；
- SST 指令设置 `SstEntry` 状态；
- rename 后的 PRE 指令进入 PRDQ，而不是 ROB；
- branch 额外记录一个周期限制。

这条通路不能直接复用为 DVR 的最终通路。DVR 要求主线程继续正常流过 ROB，同时一个独立、in-order、vectorized 子线程共享前端 buffer 和执行端口。也就是说，DVR 不是“把主线程切换进另一模式”，而是“主线程与 helper 并存”。

可以复用的部分：

- `DynInst` 增加 transient/helper 状态位的方法；
- 不进入 ROB 的投机指令生命周期；
- PRDQ/free-list 回收逻辑；
- load request 完成后清理 LSQ entry 的代码路径；
- 退出投机模式时恢复和释放资源的模式。

需要重写的部分：

- 单一 `inPRE` 状态；
- decode 丢弃主线程非 slice 指令的行为；
- 依靠正常 fetch 持续提供 helper 指令；
- PRE 和主线程互斥的资源控制。

### 3.5 物理寄存器回收

文件：`code/gem5-runahead-dev-pre/src/cpu/o3/rename.cc`

PRE 树已有：

- `prdq`；
- PRE 退出时按序释放请求与临时状态；
- 可选 early recycle；
- PRE 模式下 PRDQ 满导致 rename stall。

DVR 的 VRAT 仍然需要单独实现。论文要求一个架构寄存器在不同情况下映射为：

- 所有 lane 共享一个 scalar physical register；或
- 16 个 512-bit vector physical register 映射。

因此 PRDQ 可作为生命周期管理参考，但不能代替 VRAT。

## 4. Triangel 树已经实现了什么

### 4.1 核心预取器

README 明确指出主要实现位于：

- `src/mem/cache/prefetch/triangel.cc`
- `src/mem/cache/prefetch/triangel.hh`
- `src/mem/cache/prefetch/triage.cc`
- `src/mem/cache/prefetch/triage.hh`

其命令行配置通过以下文件接入：

- `configs/common/Options.py`
- `configs/common/CacheConfig.py`
- `src/mem/cache/prefetch/Prefetcher.py`

Triangel 是 temporal prefetcher：根据历史地址关联在 cache 侧预测未来访问。它不会执行程序指令，也没有 DVR 的 dependent chain、loop bound、SIMT lane 或 reconvergence 语义。

### 4.2 实验工程

Triangel artifact 提供了较完整的：

- `run_scripts/build.sh`
- `run_scripts/dependencies.sh`
- `run_scripts/run_experiments.sh`
- `run_scripts/analyse_experiments.sh`
- SPEC CPU2006 FS-mode checkpoint 流程
- KVM checkpoint 生成说明
- 预取 useful/unused、metadata access 等统计汇总

这些脚本的组织方式值得复用，但其 workload 与 DVR 论文不同。DVR 最终需要 GAP、Graph500、hash join、NAS 等不规则访问负载，不能直接用 Triangel 的 SPEC CPU2006 结果替代。

## 5. 两套代码不能直接互相覆盖

对 `src/cpu/o3` 的目录级差异统计表明，两树之间约有：

- 40 个 O3 文件发生差异；
- 约 592 行新增、1789 行删除；
- PRE 独有 `pre.cc` 和 `pre.hh`；
- gem5 23 新增 `O3CPU.py`、`O3Checker.py`，并重构原有 Python SimObject 定义；
- commit、cpu、rename、regfile 和 dyn_inst 等关键文件均存在大幅 API 变化。

因此不能采用以下做法：

```text
把 runahead 树的 src/cpu/o3 整个复制到 Triangel 树
```

这会同时破坏 gem5 23 的接口、统计分组和构建系统。若未来必须升级，应在 DVR 功能稳定后，将 DVR 作为一组有明确边界的 patch 逐模块移植。

## 6. DVR 在 PRE 树上的文件级映射

| DVR 机制 | 首选落点 | 说明 |
| --- | --- | --- |
| 配置参数 | `src/cpu/o3/BaseO3CPU.py` | 增加 `enableDVR`、lane 数、timeout、RPT/stack 大小等 |
| DVR 总状态 | `src/cpu/o3/cpu.hh/.cc` | 与 `inPRE` 分开；管理 Discovery、ARM、RUN、NDM、CLEANUP |
| 新控制结构 | 新建 `src/cpu/o3/dvr.hh/.cc` | RPT、VTT、FLR、LCR/SBB、loop-bound detector、reconvergence stack |
| stride 观测 | `iew.cc` / load execute 完成路径 | 需要 PC + effective address；不要只在 cache 侧观察 |
| Discovery taint | decode/rename/commit 的主线程 uop 通知 | 第一版可在 rename 后使用已解码 src/dst register ID |
| 两份寄存器快照 | rename map 访问接口 | 保存 mapping，并读取 compare 输入对应的值 |
| helper 前端 buffer | `cpu` + 新建 DVR buffer | 论文为 8 uop；第一版可先保存动态 uop 模板 |
| VRAT | 新建 DVR 类，调用 regfile/free-list 接口 | 与主线程 rename map 分离 |
| VIR/in-order issue | `iew` / `inst_queue` 邻近路径 | helper 不进入普通 IQ；主线程对同端口优先 |
| gather 拆分 | `lsq` / `lsq_unit` | 每个 lane 独立 cache request，并受 MSHR/LSQ 约束 |
| branch divergence | DVR controller + branch execute 回调 | 根据 lane next-PC 分组 mask；8 项栈 |
| helper 终止 | DVR controller | FLR、next stride PC 或 200 helper-uop timeout |
| 统计 | DVR stats group | 触发、bound、lane、prefetch、timeliness、MLP、资源阻塞 |

## 7. 推荐实现策略

### 阶段 A：保住 PRE baseline

在任何 DVR 修改前先完成：

1. 编译 `build/RISCV/gem5.opt`；
2. 运行 README 的 hello baseline；
3. 分别运行 `enablePRE=False/True`；
4. 保存配置、cycles、numCycles 和退出原因；
5. 确保修改 DVR 后 PRE 仍可运行。

PRE 不是 DVR baseline 的唯一方案，但它是验证 O3 修改没有破坏现有投机执行路径的重要回归测试。

### 阶段 B：Discovery-only

先不发任何 prefetch，只实现并打印/统计：

- stride PC、stride distance、confidence；
- 是否切换到更内层 stride；
- VTT 传播；
- FLR；
- backward branch 与 compare；
- loop bound、increment、remaining；
- fallback-to-128 次数。

这一步通过后再生成 helper load。否则地址错误很难区分来自 stride、taint、bound 还是向量化。

### 阶段 C：功能型单层 DVR

第一版允许使用简化模型：

- helper 生成的 load 直接作为 prefetch request；
- 暂不建模完整 16 个向量物理寄存器；
- 暂不实现 branch divergence 和 NDM；
- 先验证最多 128 lane 的地址和 mask；
- 所有简化必须由独立配置开关控制，并记录为 functional model。

### 阶段 D：周期与资源模型

逐项加入：

- 8-uop helper buffer；
- VIR；
- 主线程同端口优先；
- 16×AVX-512 vector copies；
- LSQ/MSHR 竞争；
- VRAT 和 vector physical register 压力；
- cache pollution 与 bandwidth。

### 阶段 E：分歧和 Nested DVR

单层模型稳定后再实现：

- 128-bit lane mask；
- 8-entry PC+mask reconvergence stack；
- 组内 scalar-to-vector mapping promotion；
- inner bound < 64 时的 NDM；
- 最多 16 个 outer lane 聚合至 128 个 inner lane。

## 8. 构建环境判断

PRE README 明确要求：

- SCons 4.3.0；
- 构建目标 `build/RISCV/gem5.opt`；
- 推荐 Linux；
- 示例是 syscall-emulation 模式；
- 可用 Clang 16 + lld 加速构建。

Triangel artifact 要求 Ubuntu 22.04、x86-64、KVM、SPEC CPU2006 和 FS checkpoints。

当前 macOS 工作区适合静态分析、写代码和小型脚本，但最终 gem5 构建与论文实验应放在 Linux 环境。右侧 `runtime-01` 更可能是正确的执行位置；在使用前需要确认：

```bash
uname -a
cat /etc/os-release
python3 --version
scons --version
g++ --version
clang++ --version
nproc
free -h
df -h
```

## 9. 当前风险与假设

1. 两套源码都缺少独立 Git 历史，无法自动分离“上游 gem5 变化”和“论文作者 patch”。
2. PRE 树面向 RISC-V，而 DVR 论文模拟 x86。先用 RISC-V 做机制原型是可行的，但不能直接称为论文级复现。
3. DVR 论文使用 Sniper 6.0，不是 gem5。选择 gem5 意味着目标是机制/趋势复现，而非逐周期完全复现。
4. 论文没有公开 DVR artifact；vector gather、端口仲裁、TLB 与 cache fill 等细节需要工程假设。
5. gem5 的 vector register 表示与论文的 16×AVX-512 映射不一定一一对应，第一版需把“逻辑 lane”与“真实 ISA vector instruction”分开。
6. 论文的 1139 B 是理论硬件存储开销，不是 C++ 对象大小。

## 10. 下一步

下一份文档应为 `docs/02_build_and_baseline.md`，并伴随实际执行：

1. 检查 `runtime-01` 的 Linux/编译环境；
2. 把 `code/gem5-runahead-dev-pre` 放到远端用户可写目录；
3. 建立 Python virtual environment，固定 SCons 4.3.0；
4. 构建 RISC-V `gem5.opt`；
5. 运行 hello 的 PRE off/on 基线；
6. 归档命令、构建日志、二进制版本和 stats；
7. 修复任何构建兼容问题，但不开始 DVR 功能实现，直到 baseline 可重复。

## 11. 主干决策记录

```yaml
decision: use gem5-runahead-dev-pre as the DVR development base
why:
  - existing O3 runahead lifecycle
  - existing transient instructions outside ROB
  - existing SST/PRDQ and register recycling
  - minimal RISC-V SE-mode example
triangel_role:
  - experiment scripting reference
  - prefetch statistics reference
  - future gem5-23 migration reference
not_now:
  - merging the two full source trees
  - x86 full-system reproduction
  - GAP-scale experiments
  - Nested DVR before single-level validation
```
