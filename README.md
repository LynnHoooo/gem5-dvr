# gem5 DVR Reproduction

> **分支 `vr-repro`**：专门复现 **Vector Runahead (ISCA 2020)** 论文
> （Naithani et al.，`docs/paper/Vector_Runahead_Ajeya.pdf`）。复现点与分阶段计划见
> [`docs/08_vr_reproduction_plan.md`](docs/08_vr_reproduction_plan.md)。
> 该分支复用 `main` 的 DVR 基座（stride 检测 / taint / VRAT/RDQ / 控制流），
> 新增 VR 特有的推测性向量化、进入/终止条件、U=P=8 展开+流水化。

A RISC-V/gem5 prototype of Decoupled Vector Runahead (DVR).

This repository contains:

- `code/gem5-runahead-dev-pre/`: gem5 source tree with the DVR implementation.
- `code/gem5-runahead-dev-pre/configs/dvr/table1_se.py`: Table 1-style single-core configuration.
- `benchmarks/dvr_*.c`: DVR stride, dependent-load, and control-flow microbenchmarks.
- `scripts/run_remote_dvr_*.sh`: build and staged smoke/regression scripts.
- `docs/02_reproduction_status.md`: current implementation and validation status.
- `docs/04_gem5_dvr_implementation.md`: implementation details and limitations.
- `code/README_DVR_REPRO.md`: source-level code guide and reproduction instructions.

## Scope

This is an ISA-adapted RISC-V/gem5 prototype. It is not a cycle-for-cycle reproduction of the paper's original Sniper/x86/AVX-512 evaluation environment or its absolute performance results.

The current implementation includes stride detection, commit-ordered discovery, register taint tracking, FLR and loop-bound inference, timing-path source/dependent prefetching, instruction recording, VRAT/VIR bookkeeping, and control-flow validation. General per-lane uop execution and fully integrated nested helper memory execution remain ongoing work; see the status documents for details.

## Reproduction

The remote scripts expect the project to be copied to the configured server workspace and run inside the project's Nix development environment. The complete staged regression is:

```bash
scripts/run_remote_dvr_regression.sh
```

For a fast structural check:

```bash
QUICK=1 scripts/run_remote_dvr_regression.sh
```

Use the full regression, rather than `QUICK=1`, as completion evidence for all stages.
