# RISC-V trigger-to-FLR replay：最新验收结果模板

> 状态：等待远端重新编译并运行 Stage 8/10 后填写。本文不预填任何运行数字。

## 验收口径

`dvr_dependent.riscv` 是专门用于命中已支持 replay evaluator 的两级依赖微基准。
因此 Stage 8 和 Stage 10 不再只检查仿射 dependent prefetch 或 VRAT/VIR 结构计数，
还必须同时满足：

- `dvrReplaySupportedUops > 0`：录制模板中确实包含 evaluator 支持的 uop；
- `dvrReplayAttempts > 0`：source response 确实进入逐 lane replay；
- `dvrReplayTargetsGenerated > 0`：replay 确实计算并提交 dependent target；
- `dvrReplayTargetsGenerated == dvrReplayAttempts`：该定向微基准中，每次合法
  replay 均成功走到 FLR load-address；
- `dvrReplayTargetsGenerated + dvrReplayFallbacks ==
  dvrSourcePrefetchesCompleted`：每个已完成 source request 都能归入 replay 成功或
  显式 affine fallback，不能静默丢失。

上述断言只适用于机制应命中的 `dvr_dependent.riscv`。Stage 11 的 divergent
workload 仍按多路径、predicate、reconvergence 和 timeout 语义验收，不强行要求
其所有路径命中当前有限 opcode 集的 replay。

## 运行命令

```bash
~/dvr-repro/scripts/run_remote_dvr_stage8_smoke.sh
~/dvr-repro/scripts/run_remote_dvr_stage10_smoke.sh
SKIP_BUILD=1 ~/dvr-repro/scripts/run_remote_dvr_regression.sh
```

## 可粘贴到 README 的结果段落

以下占位符只能用远端脚本成功退出后打印的值替换：

````markdown
### 逐 lane trigger-to-FLR replay 验收

重新编译后的 gem5 已通过 `dvr_dependent.riscv` 的 Stage 8 和 Stage 10
硬性回归。该回归不仅要求产生 dependent cache timing request，还要求 source
response 真正执行录制的 RISC-V uop 模板，并对每个 source response 做成功/
fallback 守恒核算。

```text
DVR_STAGE8_SMOKE_PASSED ...
replay_supported=<STAGE8_SUPPORTED>
replay_attempts=<STAGE8_ATTEMPTS>
replay_targets=<STAGE8_TARGETS>
replay_fallbacks=<STAGE8_FALLBACKS>

DVR_STAGE10_SMOKE_PASSED ...
replay_supported=<STAGE10_SUPPORTED>
replay_attempts=<STAGE10_ATTEMPTS>
replay_targets=<STAGE10_TARGETS>
replay_fallbacks=<STAGE10_FALLBACKS>
```

其中 `replay_targets == replay_attempts`，且
`replay_targets + replay_fallbacks == source_completed`。这证明当前定向微基准的
合法模板使用真实逐 lane uop replay 生成 FLR 地址；fallback 仍保留给无法安全
重放的模板。该结果不等同于支持任意 RISC-V opcode 或任意依赖图。
````

## 失败时定位

```bash
grep -E 'dvrReplay(SupportedUops|UnsupportedUops|UnstableInputs|Attempts|TargetsGenerated|Fallbacks)|dvrSourcePrefetchesCompleted' \
  ~/dvr-repro/results/dvr-stage8-dependent-prefetch/stats.txt
```

- `SupportedUops == 0`：录制范围或指令解码未命中支持集；
- `Attempts == 0`：模板被判 invalid，优先看 `UnsupportedUops` 和
  `UnstableInputs`；
- `TargetsGenerated < Attempts`：有合法模板在 evaluator 中途失败，需检查寄存器
  定义链或 FLR load-address 是否为最后可执行节点；
- 守恒式不成立：source completion 存在未计入成功/fallback 的控制路径，不能将
  本轮结果写入 README 的“已通过”部分。
