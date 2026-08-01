# gem5 RISC-V DVR / PRE

本仓库是基于 gem5 O3 CPU 的 RISC-V 机制研究代码，包含：

- PRE（Precise Runahead Execution）相关实现；
- DVR（Decoupled Vector Runahead）的 RISC-V ISA-adapted 机制原型。

## DVR 复现文档

完整的实现导览、代码阅读顺序、Stage 1–14 验证证据、服务器构建命令、回归脚本、
当前缺口和论文表述边界，请以仓库外层文档为准：

[`../README_DVR_REPRO.md`](../README_DVR_REPRO.md)

该文档是本项目 DVR 复现状态的权威记录，不要仅根据本 README 判断某项机制是否
已经完成。

## 代码入口

```text
src/cpu/o3/pre.hh / pre.cc       DVR 数据结构与核心算法
src/cpu/o3/cpu.hh / cpu.cc       O3 CPU 集成、helper 请求和统计
src/cpu/o3/lsq_unit.cc           主线程 load observation
src/cpu/o3/lsq.cc                DVR response 路径
src/cpu/o3/dvr_predicate.*       predicate mask 与路径状态
src/cpu/o3/dvr_quality.*         质量指标事件 tracker
configs/dvr/table1_se.py         Table 1 风格配置
```

## 分支约定

```text
main                         稳定代码
docs/dvr-reproduction-plan   DVR 开发、验证和实验代码
```

DVR 修改应先在开发分支完成，并通过服务器上的对应 Stage 和回归脚本验证；验证
通过后再合并到 `main`。

## 基本编译入口

具体环境变量和服务器流程见 [`README_DVR_REPRO.md`](../README_DVR_REPRO.md)。在已
配置好 gem5 环境后，基本命令为：

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

## 重要边界

当前项目是 **RISC-V ISA-adapted DVR 机制原型**，不是原论文 x86/Sniper 绝对性能
数字的逐项复刻。正式实验和论文表述必须遵循
[`README_DVR_REPRO.md`](../README_DVR_REPRO.md) 中记录的已完成部分与已知限制。
