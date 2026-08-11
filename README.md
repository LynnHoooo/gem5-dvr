# gem5-dvr

这是一个在 **PRE/gem5** 基础上实现 DVR（Decoupled Vector Runahead）的 RISC-V
机制原型。它的目标是验证 DVR 的 Discovery、依赖链 replay、真实 cache 请求和
资源竞争路径；它不是论文作者未公开 Sniper 实现的逐周期、逐位复刻。

```text
gem5-dvr/
├── code/
│   ├── README_DVR_REPRO.md        # 详细实现、验证和服务器操作说明
│   └── gem5-runahead-dev-pre/     # PRE/gem5 源码树，DVR 主要修改在这里
├── benchmarks/                    # DVR 微基准
├── scripts/                       # 构建、Stage smoke 和回归脚本
├── configs/                       # 参考配置
├── DVR_EXPERIMENT_VALIDATION_PLAN.md # 分层验收和实验结果
├── DVR_DEBUG_20260809.md          # 日期化调试/实验记录
└── code/docs/09_camel_validation_report.md # Camel 验证报告
```

## 代码入口

主要代码位于：

```text
code/gem5-runahead-dev-pre/src/cpu/o3/
```

重点文件：

```text
pre.hh / pre.cc       Stride、Discovery、VTT、FLR、VRAT、VIR
cpu.hh / cpu.cc       DVR 集成、helper、prefetch request 和统计
dvr_nested.*          Nested DVR
dvr_predicate.*       lane predicate 和 reconvergence
dvr_quality.*         质量统计
```

## 快速开始

以下命令从仓库根目录运行。服务器环境可通过 `VENV` 指定带 SCons 的兼容 Python
虚拟环境（当前 gem5 树要求 Python 3.8–3.12）；`ROOT` 显式指向本仓库的 gem5
源码，避免误用服务器上的其他 checkout。

```bash
git clone https://github.com/LynnHoooo/gem5-dvr.git
cd gem5-dvr

ROOT="$PWD/code/gem5-runahead-dev-pre" \
VENV=/path/to/python-3.8-to-3.12-venv \
JOBS=32 \
bash scripts/build_remote_gem5_dvr.sh
```

快速回归：

```bash
ROOT="$PWD/code/gem5-runahead-dev-pre" \
SKIP_BUILD=1 QUICK=1 \
bash scripts/run_remote_dvr_regression.sh
```

完整回归会运行多个 gem5 workload，不会自动执行：

```bash
ROOT="$PWD/code/gem5-runahead-dev-pre" \
bash scripts/run_remote_dvr_regression.sh
```

更详细的命令、Stage 证据和已知缺口请查看：

[`code/README_DVR_REPRO.md`](code/README_DVR_REPRO.md)

实验必须从第一层 gate 开始。M1/M2/M3 未通过时，不应把后续 Figure 8 或 GAP
结果称为完整 DVR reproduction：

[`DVR_EXPERIMENT_VALIDATION_PLAN.md`](DVR_EXPERIMENT_VALIDATION_PLAN.md)

Camel 的独立验证报告：

[`code/docs/09_camel_validation_report.md`](code/docs/09_camel_validation_report.md)

## 当前定位

这是 **RISC-V ISA-adapted DVR prototype**，不是原论文 x86/AVX-512 环境的逐周期
复现。实验结果应以 `code/README_DVR_REPRO.md` 中记录的已验证内容为准。

截至 2026-08-11，Camel `MAX_KEY=65536` 的固定 ROI 已验证：Baseline/Full 的
committed instructions 均为 `5,767,296`，Full 为 `1.6514x`；其主链 L1D demand
miss 从 `73,836` 降至 `7,123`，且没有 translation fault。该结果证明普通
Discovery + source/dependent replay 路径有效，不代表全部 DVR 工作负载或论文绝对
性能均已复现。

当前尚未关闭的边界包括：helper 不是完整的 gem5 O3 `DynInst` 流；NDM 和
分支重汇聚只在专项测试中覆盖；以及跨 Discovery alternate-path cache 的最新增量
需要在本提交构建后再完成专项回归。详见复现文档的“发布快照”章节。

服务器在 2026-08-11 仅发现 Python 3.13 虚拟环境，当前 gem5 配置拒绝该版本；
旧的 Python 3.11 Nix 路径也已失效。因此本次提交已通过文档/脚本静态检查，
但新增 alternate-path 增量的 gem5 编译与专项回归仍待在兼容 Python 环境中执行。

## 分支与生成文件

```text
main                         当前集成版本
```

`*.riscv`、`results/`、`m5out/` 和 gem5 `build/` 都是本地生成物，不提交到
Git。微基准只保存源文件，由验证脚本使用固定工具链重新构建。服务器绝对路径和
Nix store 路径不是代码接口，应通过 `ROOT`、`VENV`、`PYTHON_ROOT`、`ZLIB_DEV` 和
`ZLIB_LIB` 覆盖。
