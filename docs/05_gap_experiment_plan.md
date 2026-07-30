# 缩小版 GAP 实验准备与缺口

更新时间：2026-07-30

## 当前结论

目前不能声称 GAP 已在 RISC-V DVR 原型上运行。

本地工作区检查结果：

- 没有 GAP Benchmark Suite（GAPBS）源码目录；
- 没有 RISC-V 版 `bc`、`bfs`、`cc`、`pr`、`sssp` 可执行文件；
- 没有 `.gr`、`.sg` 或 `.wsg` 图输入；
- `code/gem5-runahead-dev-pre/configs/example/gem5_library/` 中只有
  `x86-gapbs-benchmarks.py`。该脚本要求 X86、KVM、MESI Two Level、Linux
  kernel 和 `x86-gapbs` 磁盘镜像，不能复用为当前 RISC-V SE/Table-1 DVR 对照；
- `mobile-hmp-gem5/graphbench` 属于另一套移动异构图实验，不是 GAPBS 的
  RISC-V DVR workload。

当前本地工作区和远端 DVR 项目资源检查均未找到可直接运行的 GAPBS
RISC-V 二进制及图输入。若服务器资源后来发生变化，应先执行下一节的只读命令
重新核验，再更新本结论。

## 先在服务器核验现有资源

在能够登录 `pre` 的终端运行：

```bash
find ~/dvr-repro ~/.cache/gem5 ~/gem5-resources ~/resources \
  -maxdepth 6 -type f \
  \( -name bc -o -name bfs -o -name cc -o -name pr -o -name sssp \
     -o -name '*.gr' -o -name '*.sg' -o -name '*.wsg' \) \
  -print 2>/dev/null

file ~/dvr-repro/source/gem5-runahead-dev-pre/benchmarks/* 2>/dev/null

command -v riscv64-unknown-linux-gnu-g++
command -v riscv64-linux-gnu-g++
```

可执行文件必须由 `file` 确认为 RISC-V ELF；x86 GAP 镜像中的程序不能交给
`build/RISCV/gem5.opt` 的 SE workload。

## 精确依赖

最小流程需要：

1. GAPBS 源码，固定一个 commit SHA 并在实验报告中记录；
2. RV64 Linux GNU C++ cross compiler；
3. 目标 sysroot 中的静态 `libstdc++`、`libgcc`、glibc/pthread，或者一个确认能被
   gem5 SE 支持的动态 loader 与完整 RISC-V shared-library 路径；
4. 当前 DVR 构建：
   `~/dvr-repro/source/gem5-runahead-dev-pre/build/RISCV/gem5.opt`；
5. 当前配置：
   `configs/dvr/table1_se.py`；
6. smoke 阶段可用 GAPBS 内建 Kronecker generator，避免先下载大图。正式论文
   对比仍需固定 Kron、LiveJournal、Orkut、Twitter、Urand 的版本和校验和。

## 建议构建命令

以下命令应在 `~/buckyball` 的 `nix develop` 环境中执行。先根据
`command -v` 选择实际存在的工具链前缀：

```bash
cd ~/dvr-repro/source
git clone https://github.com/sbeamer/gapbs.git
cd gapbs
git rev-parse HEAD | tee GAPBS_COMMIT

make clean
make -j"$(nproc)" \
  CXX=riscv64-unknown-linux-gnu-g++ \
  CXX_FLAGS='-std=c++11 -O3 -Wall -static'

mkdir -p ~/dvr-repro/source/gem5-runahead-dev-pre/benchmarks/gapbs-riscv
cp bc bfs cc pr sssp \
  ~/dvr-repro/source/gem5-runahead-dev-pre/benchmarks/gapbs-riscv/

file ~/dvr-repro/source/gem5-runahead-dev-pre/benchmarks/gapbs-riscv/*
```

若环境提供的是 `riscv64-linux-gnu-g++`，只替换 `CXX`。若链接器报告缺少静态
`libstdc++`/pthread，不应去掉 `-static` 后直接假定可运行；应先安装对应 RISC-V
sysroot，或显式给 SE 模式提供目标动态 loader 和 libraries。

## 首个可复制 smoke

先只用 BFS、scale 10、一次 trial。该输入用于流程和统计字段验证，不用于论文
性能结论：

```bash
ROOT=~/dvr-repro/source/gem5-runahead-dev-pre
BIN="$ROOT/benchmarks/gapbs-riscv/bfs"
RESULTS=~/dvr-repro/results/gap-smoke

test -x "$ROOT/build/RISCV/gem5.opt"
test -x "$BIN"
file "$BIN" | grep -qi 'RISC-V'

rm -rf "$RESULTS/baseline" "$RESULTS/dvr"
mkdir -p "$RESULTS/baseline" "$RESULTS/dvr"

"$ROOT/build/RISCV/gem5.opt" --outdir="$RESULTS/baseline" \
  "$ROOT/configs/dvr/table1_se.py" \
  --cmd="$BIN" --options='-g 10 -n 1'

"$ROOT/build/RISCV/gem5.opt" --outdir="$RESULTS/dvr" \
  "$ROOT/configs/dvr/table1_se.py" \
  --cmd="$BIN" --options='-g 10 -n 1' --dvr

awk '$1 == "system.cpu.numCycles" ||
     $1 == "system.cpu.dcache.ReadReq.misses::cpu.data" {print}' \
  "$RESULTS/baseline/stats.txt" "$RESULTS/dvr/stats.txt"
```

在加入自动脚本前，必须确认两个进程均正常退出、程序输出一致、统计非空，且没有
SE syscall fatal。之后再把 scale 从 10 调到能在三小时时限内完成但超过 LLC
容量的最小值。

## 扩展矩阵

smoke 通过后固定二进制、输入、maxinsts/ROI 和随机种子，再执行：

| 维度 | 最小集合 |
|---|---|
| workload | bc、bfs、cc、pr、sssp |
| 输入 | 内建 Kronecker 小图；随后固定真实图 |
| 机制 | Baseline、PRE、Offload/Discovery、Nested DVR |
| 重复 | 至少 3 次；gem5 确定性运行也保留日志和配置 |
| 核心指标 | cycles、instructions、IPC、demand L1D/LLC misses |
| DVR 指标 | trigger/discovery、lanes、generated/issued/completed/dropped |
| 预取质量 | accuracy、coverage、timeliness、bandwidth、pollution |

最后一行预取质量指标当前尚未完整实现；在指标实现前，GAP 只能作为功能/周期
探索，不能形成论文完整消融结论。

## 通过门槛

只有同时保存以下证据，文档才能写“GAP smoke 已跑”：

1. GAPBS commit SHA、交叉编译命令和 RISC-V ELF 的 `file` 输出；
2. 完整 gem5 命令、`config.ini`、`stats.txt` 和 stdout/stderr；
3. baseline 与 DVR 使用相同二进制、参数和 Table-1 配置；
4. 两组均正常退出，benchmark 输出语义一致；
5. cycles 和 demand misses 能由脚本非空读取；
6. 不把 scale-10 synthetic smoke 的结果外推为论文五图或 500M ROI 结果。
