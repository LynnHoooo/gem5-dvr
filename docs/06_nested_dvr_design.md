# Nested Discovery / DVR：两层控制状态机

## 当前实现边界

`src/cpu/o3/dvr_nested.hh` 和 `dvr_nested.cc` 实现了一个最多两层的
Nested Discovery 控制器，并已加入 O3 `SConscript`。它是真实的状态管理，
并已真实接入 O3 CPU 的提交生命周期：外层 discovery 建立 root frame；执行段
发现的内层 stride candidate 必须等同一条动态 load 提交后才建立 child frame；
child 在内层 trigger 再次提交时完成，也受提交指令预算限制。2026-07-30 的
Stage 13 已进一步接入独立 child 执行上下文和真实 helper 内存请求，并通过
`generated/issued/completed` 三重硬断言。

## 状态与约束

- 空栈时 `startRoot()` 建立 depth 1 frame。
- `startNested(parentId, ...)` 只接受当前栈顶作为父节点，并建立 depth 2 frame。
- 第三层被 `RejectedDepth` 明确拒绝。
- `complete()` 必须按 LIFO 顺序完成；试图先完成父节点返回
  `RejectedOrder`，不会破坏现场。
- `observeCommit(pc, sequence)` 先检查 child trigger 的非起始动态实例重现，再给
  所有存活 frame 增加 committed-instruction age。
  栈顶达到预算后产生 `TimedOut` 并只弹出栈顶；父层保留，后续提交可独立
  完成或 timeout。
- `reset()` 清除活动 frame、保留 lifetime counters；`clear()` 同时清计数。

## 可直接映射到 gem5 statistics 的接口

`statistics()` 返回：

- `rootStarts`, `nestedStarts`
- `rootCompletions`, `nestedCompletions`
- `rootTimeouts`, `nestedTimeouts`
- `depthRejects`, `parentRejects`, `orderRejects`

每个 API 同时返回结构化事件，含 discovery ID、parent ID、depth、trigger PC、
FLR PC 和提交指令数。ID 单调递增，避免仅靠 PC 混淆动态实例。

## 已完成的 CPU 接线

1. `CPU` 持有 `DVRNestedController`，预算复用 `dvrDiscoveryMaxInsts`。
2. 普通 discovery 真正开始时调用 `startRoot()`；发现阶段中的另一个 confident
   trigger 先保存 `(PC, sequence)`，只有精确匹配的动态 load 提交才调用
   `startNested()`，从而排除被 squash 的候选。
3. 每条架构指令提交时调用 `observeCommit(pc, sequence)`；完成和 timeout 事件
   已映射到 gem5 statistics。
4. root 结束时，未结束的 child 被显式 reset，并由
   `dvrNestedParentResets` 计数，不会泄漏到下一轮 discovery。

## 已完成的 child 执行上下文接线

1. `DVRNestedExecutionContext` 独立持有 taint、loop-bound、recorder、VRAT、VIR、
   register checkpoints、trigger value/address/stride 和动态 discovery ID；child
   reset 不修改 root 的对应对象。
2. child recurrence 提交时先完成旧 child，再允许同一动态 load 启动下一代
   child，避免覆盖尚未物化的 replay template。
3. child 使用自己的 recorder/checkpoint 构建 replay template，并将最多 128 个
   source lane 以 append-only 方式加入共享 helper memory queue；不会调用 root 的
   supersede/clear 路径。
4. 请求仍经过主线程优先的 data-port 仲裁。`dvrNestedHelpersGenerated/Issued/
   Completed` 只对带 nested 标志的请求计数。

地址 relation predictor 和最终的物理 helper queue 仍由 root/child 共享；独立的是
discovery/replay 的可变机制状态。这是共享预测器资源的设计选择，不应描述成每层
拥有完全独立的硬件后端。

## 2026-07-30 远端证据

- 完整 RISC-V gem5 SCons 编译和链接通过。
- Stage 11 控制流回归通过。
- `dvrNestedRootStarts=8520`
- `dvrNestedStarts=2931`
- `dvrNestedCompletions=6`
- `dvrNestedParentResets=2924`
- `dvrNestedTimeouts=0`，`dvrNestedDepthRejects=0`

上述是接入执行上下文之前的 Stage 11 控制器证据。接入后的专用 Stage 13 为：

```text
DVR_STAGE13_NESTED_PASSED contexts=440 programs=2 vrat=80 vir=80
generated=256 issued=251 completed=251
all_programs=31165 all_helpers=3011226
```

`dvr_nested.c` 同时提供明确的 outer 和 inner trigger-to-FLR 链。该结果证明两个
child program 由独立 VRAT/VIR 物化，生成 256 个 child source lanes，其中 251 个
经 L1D timing port 接受并全部返回；其余 5 个被主线程优先仲裁抑制。它仍是机制
微基准证据，不等同于 GAP 或论文全套 benchmark 的 Nested 性能复现。

## Stage 14：NDM 控制语义第一阶段

在现有两层 controller 之外新增 `DVRNestedDiscoveryMode`，用于明确表达论文
Nested Discovery Mode 的控制入口，而不把普通 child discovery 误称为完整 NDM。
当前阶段实现：

- 可配置 `dvrNDMThreshold`，默认 64；只有可信 inner lane count 小于阈值时启动；
- 独立 `dvrNDMMaxInsts` 提交预算，默认 512，不需要缩短普通 Discovery；
- 保存 Inner Load Register（inner trigger PC）、Increment Register（loop increment）
  和 inner lane count；
- 只接受不同于 inner trigger、且经过精确动态 load commit 过滤的 outer stride；
- commit-budget timeout 和显式 ordinary-DVR fallback；
- `dvrNDMAttempts`、`dvrNDMOuterFound`、`dvrNDMFallbacks`、
  `dvrNDMTimeouts` 统计；
- Stage 14 正常、阈值禁用和 timeout 三组验收入口。

本阶段仍不包含 branch-direction inversion、outer-lane vectorization、逐 outer
invocation 的 inner bound 收集或 flatten-to-128，因此不能描述为论文 4.3 的完整
NDM 实现。它为下一阶段提供了可提交排序、可超时、可观察的控制状态。

专用入口：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage14_ndm_control.sh
```

## 最小验证矩阵

| 场景 | 预期 |
|---|---|
| root start → complete | root starts/completions 各 1，depth 回到 0 |
| root → child → child complete → root complete | nested starts/completions 各 1，严格 LIFO |
| root → child → third level | `RejectedDepth`，depth 保持 2 |
| root → child → complete(root) | `RejectedOrder`，两个 frame 均保留 |
| child 达预算 | child timeout，root 保留 |
| root 达预算 | root timeout，depth 回到 0 |

专用入口：

```bash
~/dvr-repro/scripts/run_remote_dvr_stage13_nested.sh
```

脚本对 context、program、VRAT、VIR、generated、issued、completed 逐项要求非零；
任何一项缺失都会返回失败。
