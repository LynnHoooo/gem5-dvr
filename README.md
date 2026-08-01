# gem5-dvr

这是一个基于 gem5 O3 CPU 的 **DVR（Decoupled Vector Runahead）RISC-V 机制原型**。

项目的代码不是从零开始编写的，而是在 PRE（Precise Runahead Execution）gem5
仓库基础上扩展 DVR 机制：

```text
gem5-dvr repository
└── code/gem5-runahead-dev-pre/    PRE/gem5 源码树 + DVR 修改
```

当前项目用于机制验证、消融实验和后续 workload 实验。它是 RISC-V ISA-adapted
原型，不是原论文 Sniper/x86/AVX-512 环境的逐周期复刻，也不应直接复现论文中的
绝对性能数字。

## 从哪里开始

如果第一次阅读项目，建议按以下顺序：

1. [DVR 代码与复现导览](code/README_DVR_REPRO.md)
2. [当前复现状态](docs/02_reproduction_status.md)
3. [DVR gem5 实现说明](docs/04_gem5_dvr_implementation.md)
4. [缺口与实验计划](docs/05_gap_experiment_plan.md)
5. [Nested DVR 设计](docs/06_nested_dvr_design.md)
6. [质量指标说明](docs/07_dvr_quality_metrics.md)

`code/README_DVR_REPRO.md` 是实现状态、Stage 验证证据、服务器命令和已知限制的
权威记录。README 中的简要介绍不能替代该文档。

## 项目结构

```text
gem5-dvr/
├── README.md                         # 项目入口和文件导航
├── code/
│   ├── README_DVR_REPRO.md           # 详细复现导览、验证证据和服务器流程
│   ├── gem5-runahead-dev-pre/       # PRE/gem5 源码树，DVR 主要改动位于此
│   │   ├── src/cpu/o3/               # DVR 微结构和 O3 CPU 集成
│   │   ├── configs/dvr/              # gem5 DVR 配置
│   │   ├── tests/                    # gem5 测试和 smoke test
│   │   └── README.md                 # 原 PRE/gem5 仓库说明
│   └── gem5-triangel-stable/         # 独立参考代码树，不是 DVR 主实现
├── benchmarks/                       # RISC-V DVR 微基准
│   ├── dvr_stride.c                  # stride 检测和 Discovery
│   ├── dvr_dependent.c               # 两级依赖 load/replay
│   ├── dvr_divergent.c               # predicate/divergence
│   ├── dvr_nested.c                  # Nested DVR
│   ├── dvr_nested_variable.c         # 不同 inner bound 的 Nested DVR
│   └── dvr_ndm.c                     # NDM 控制测试
├── configs/
│   └── sniper/dvr_table1_baseline.cfg # Sniper/Table 1 参考配置
├── scripts/                          # 本地/服务器构建、smoke 和回归脚本
│   ├── build_remote_gem5_dvr.sh
│   ├── run_remote_dvr_stage*.sh
│   ├── run_remote_dvr_ablation.sh
│   └── run_remote_dvr_regression.sh
└── docs/                             # 设计、状态、实验和论文相关文档
```

## DVR 代码入口

主要源码位于 `code/gem5-runahead-dev-pre/src/cpu/o3/`：

| 文件/模块 | 作用 |
| --- | --- |
| `pre.hh / pre.cc` | Stride Detector、Discovery、VTT、FLR、Loop Bound、VRAT、VIR |
| `dvr_nested.hh / .cc` | Nested Controller 和 NDM 控制状态 |
| `dvr_predicate.hh / .cc` | 基于 source response 的 lane predicate/path mask |
| `dvr_quality.hh / .cc` | 预取质量事件和统计 |
| `cpu.hh / cpu.cc` | Discovery 生命周期、helper、请求队列、replay 和统计 |
| `lsq_unit.cc` | 主线程 load observation 和数据端口交互 |
| `lsq.cc` | DVR cache response 路径 |
| `BaseO3CPU.py` | DVR 参数定义 |
| `configs/dvr/table1_se.py` | Table 1 风格的 gem5 SE 配置 |

当前实现覆盖的主要机制包括：

```text
RPT stride detection
Discovery / VTT / FLR
loop-bound and lane-count inference
VRAT / VIR
event-driven decoupled helper
source/dependent cache timing request
predicate mask and reconvergence prototype
Nested invocation batch and inner-lane flatten prototype
```

各机制的“已实现”和“已验证”不是同一个概念，请以
[`README_DVR_REPRO.md`](code/README_DVR_REPRO.md) 中的状态表为准。

## 编译

服务器使用的环境、依赖版本和完整构建命令记录在
[`README_DVR_REPRO.md`](code/README_DVR_REPRO.md) 中。进入 gem5 源码树后，基本
构建目标为：

```bash
cd code/gem5-runahead-dev-pre
scons build/RISCV/gem5.opt -j$(nproc)
```

不要在没有确认环境的情况下直接复制其他机器的构建命令；如果遇到环境问题，
请把成功的环境、变量和命令记录回复现文档。

## 运行验证

快速结构检查：

```bash
QUICK=1 scripts/run_remote_dvr_regression.sh
```

完整回归：

```bash
scripts/run_remote_dvr_regression.sh
```

单独运行某一阶段：

```bash
scripts/run_remote_dvr_stage8_smoke.sh
scripts/run_remote_dvr_stage9_compare.sh
scripts/run_remote_dvr_stage11_control_flow.sh
scripts/run_remote_dvr_stage13_nested.sh
scripts/run_remote_dvr_stage15_resource_smoke.sh
```

脚本的远端工作目录、输出目录和 Stage 验收条件以脚本本身及
`code/README_DVR_REPRO.md` 为准。

## 当前实验边界

当前项目适合用于：

- RISC-V 上的 DVR 机制原型验证；
- stride、dependent replay、predicate 和 Nested DVR 微基准；
- Baseline / VR-like / Offload / Discovery / DVR / Nested DVR 消融；
- GAP 图负载的初步实验。

当前不能直接声称：

- 完整复现原论文的 x86/AVX-512 硬件；
- 完整复现论文的 Sniper 时序和绝对 speedup；
- 所有 RISC-V opcode 都能被逐 lane DVR evaluator 执行；
- Nested NDM 和 branch reconvergence 已达到论文完整语义。

## 分支和文件管理

```text
main                         稳定版本
docs/dvr-reproduction-plan   DVR 开发、验证和实验分支
```

DVR 修改应在开发分支完成，并先在服务器验证，再合并到 `main`。不要使用
`git add .`，避免把 PDF、PPT、其他 gem5 树和实验结果误提交到仓库。
