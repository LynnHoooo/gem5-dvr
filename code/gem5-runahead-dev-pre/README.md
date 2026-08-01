# gem5 Runahead / DVR

这是一个基于 gem5 的 RISC-V 研究原型，包含两条相关路线：

1. **PRE（Precise Runahead Execution）**：复现 HPCA 2020 的 Precise Runahead Execution。
2. **DVR（Decoupled Vector Runahead）**：在 RISC-V O3 CPU 上实现 DVR 风格的发现、向量依赖执行和辅助预取。

当前仓库用于机制开发和服务器实验，不代表对原论文 x86/AVX-512 模型的周期精确复现。

## 分支说明

| 分支 | 用途 |
| --- | --- |
| `main` | 稳定版本和已验证代码 |
| `docs/dvr-reproduction-plan` | DVR 开发、调试和实验代码 |

建议先在 DVR 分支验证，确认结果后再合并到 `main`。

## DVR 当前实现

代码主要位于 `src/cpu/o3/`：

| 模块 | 作用 |
| --- | --- |
| `DVRStrideDetector` | 识别稳定 stride load |
| `DVRDiscoveryController` | 管理 Discovery 阶段和超时 |
| `DVRVectorTaintTracker` | 追踪依赖值传播并识别 FLR |
| `DVRLoopBoundDetector` | 推断循环边界和 lane 数 |
| `DVRInstructionRecorder` | 记录 trigger 到 FLR 的 uop 模板 |
| `DVRVectorRenameTable` | 管理向量寄存器到物理寄存器的映射 |
| `DVRVectorInstructionRegister` | 执行私有向量寄存器中的支持 uop |
| `DVRHelperThread` | 调度 DVR helper 的请求和生命周期 |
| `DVRLanePredicateTracker` | 记录 lane predicate 和路径发散 |

DVR helper 不修改主线程的架构寄存器状态。它在主线程访问 LSQ 数据端口之后发射辅助请求，并通过共享缓存层次为主线程提前获取数据。

当前 VIR 支持的基本语义包括：

```text
ADD / ADDI / SLLI / ANDI / LOAD_ADDRESS
```

当前实现已经包含 active mask、deferred path、reconvergence stack 和基本 Nested DVR 控制框架，但仍属于功能性 RISC-V 原型。

## 编译

建议使用固定版本的 SCons：

```bash
pip3 install scons==4.3.0
```

编译 RISC-V gem5：

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

如果使用 Clang：

```bash
CXX=clang++ scons build/RISCV/gem5.opt -j$(nproc) --linker=lld
```

## DVR 配置与运行

主要配置文件：

```text
configs/dvr/table1_se.py
```

该配置近似 DVR 论文 Table 1 的单核、三级缓存参数，并提供 DVR、Nested DVR、helper uop 数量和 Discovery 超时等选项。

查看参数：

```bash
build/RISCV/gem5.opt configs/dvr/table1_se.py --help
```

示例：

```bash
build/RISCV/gem5.opt \
  --outdir=m5out-dvr \
  configs/dvr/table1_se.py \
  --dvr \
  --cmd=/path/to/rv64-benchmark
```

具体参数以当前 `table1_se.py --help` 输出为准。

## 预实验建议

至少保存以下结果：

| 类别 | 关注指标 |
| --- | --- |
| 性能 | `numCycles`、IPC、运行时间 |
| 预取效果 | coverage、timeliness、possibly useful、late |
| 资源影响 | helper 请求数、MSHR/带宽压力、主线程抑制次数 |
| 控制流 | divergent branches、reconvergences、stack overflow |
| 稳定性 | discovery timeout、replay fallback、unsupported uop |

建议使用以下对照组：

```text
OoO baseline
普通硬件预取器
DVR disabled
DVR enabled
Nested DVR enabled
```

## 已知限制

当前版本不是论文级严格复现，主要限制如下：

- 使用 RISC-V，而原 DVR 论文基于 x86/AVX-512；
- helper 是事件驱动的解耦执行模型，不是完整的独立 fetch/decode/issue 硬件流水线；
- VIR 只支持有限的整数地址生成语义；
- branch target 和 reconvergence PC 已建立基本状态，但复杂多路径控制流仍需进一步验证；
- Nested DVR 已有控制框架，完整的跨 inner-loop flatten 仍在开发中；
- 配置和缓存时序用于机制对比，不应直接与论文数值一一对应。

论文或汇报中建议使用以下表述：

> A functional RISC-V prototype of DVR-style decoupled vector prefetching.

## PRE 相关运行

PRE 的参数位于 `BaseO3CPU`，包括：

| 参数 | 含义 |
| --- | --- |
| `enablePRE` | 开启 PRE |
| `enablePREBranch` | 允许 PRE 中的 branch |
| `enablePREEarlyRecycle` | 开启寄存器提前回收 |
| `numPRDQEntries` | 精确寄存器回收队列大小 |
| `numSSTEntries` | Stalling Slice Table 大小 |

示例：

```bash
build/RISCV/gem5.opt \
  --outdir=m5out-pre \
  examples/three_level_o3.py \
  -p system.cpu.enablePRE=True \
  tests/test-progs/hello/bin/riscv/linux/hello
```

## 目录速览

```text
configs/dvr/        DVR 实验配置
src/cpu/o3/pre.*    PRE/DVR 核心机制
src/cpu/o3/cpu.*    O3 CPU 集成、helper 请求和统计
tests/              gem5 测试与 DVR smoke test
build/              gem5 编译输出（本地生成）
```
