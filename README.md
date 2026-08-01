# gem5-dvr

这是一个在 **PRE/gem5** 基础上实现 DVR（Decoupled Vector Runahead）的 RISC-V
机制原型。

```text
gem5-dvr/
├── code/
│   ├── README_DVR_REPRO.md        # 详细实现、验证和服务器操作说明
│   └── gem5-runahead-dev-pre/     # PRE/gem5 源码树，DVR 主要修改在这里
├── benchmarks/                    # DVR 微基准
├── scripts/                       # 构建、Stage smoke 和回归脚本
├── configs/                       # 参考配置
└── docs/                          # 设计、实验和状态文档
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

## 编译和验证

```bash
cd code/gem5-runahead-dev-pre
scons build/RISCV/gem5.opt -j$(nproc)
```

快速回归：

```bash
QUICK=1 scripts/run_remote_dvr_regression.sh
```

完整回归：

```bash
scripts/run_remote_dvr_regression.sh
```

更详细的命令、Stage 证据和已知缺口请查看：

[`code/README_DVR_REPRO.md`](code/README_DVR_REPRO.md)

## 当前定位

这是 **RISC-V ISA-adapted DVR prototype**，不是原论文 x86/AVX-512 环境的逐周期
复现。实验结果应以 `README_DVR_REPRO.md` 中记录的已验证内容为准。

## 分支

```text
main                         稳定版本
docs/dvr-reproduction-plan   DVR 开发和验证版本
```
