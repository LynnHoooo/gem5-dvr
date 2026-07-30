# DVR 严格质量指标与接线契约

## 为什么单独实现

此前 CPU 中的 `dvrPrefetchesPossiblyUseful` 和 `dvrPrefetchesLate` 只根据
虚拟 cache line 与 outstanding/completed 请求集合估计，不能观察 cache tag
命中、填充和替换，因此只能称为 proxy。本模块
`DVRQualityTracker` 不会把请求延迟猜成 cache 行为；缺少真实 cache 事件时，
对应严格指标保持不可报告。

代码：

- `src/cpu/o3/dvr_quality.hh`
- `src/cpu/o3/dvr_quality.cc`
- `src/cpu/o3/dvr_quality_smoke.cc`

## 可证定义

| 指标 | 定义 | 必需事件 |
|---|---|---|
| issued accuracy | 首次被 demand 命中的及时 DVR 行数 / DVR issued 请求数 | helper issue、实际 tag lookup |
| fill accuracy | 首次被 demand 命中的及时 DVR 行数 / 实际 DVR cache fill 数 | fill、实际 tag lookup |
| coverage | 原本会在 demand-only shadow miss、但被 DVR 及时命中的行数 / demand-only shadow cache miss 数 | 实际 demand 地址；同几何 LRU shadow |
| timeliness | timely useful / (timely useful + late useful) | 实际 tag lookup、helper outstanding/fill |
| bandwidth | issued/completed 请求数与字节数 | helper issue/response 的真实请求大小 |
| pollution eviction | DVR fill 直接逐出的 demand-origin resident 行数 | fill victim 及其来源 |
| pollution miss | 上述 victim 在 demand-only shadow 中仍命中、但实际 cache miss | fill victim、实际 lookup、shadow |

`leadTime` 是及时填充时刻到第一次 demand 使用时刻的 tick 差总和；
`averageLeadTime = leadTime / usefulTimely`。

## 事件顺序契约

1. helper 发出时调用 `issued(id, line, bytes, tick)`；返回时调用
   `completed(id, tick)`。这里的 line 必须是 MMU 翻译后的物理 block
   address，才能与 cache probe 的 line 匹配；CPU issue 接线已改用
   `req->getPaddr()`。
2. 每次 demand tag 查询之后、安装 demand fill 之前调用
   `demandLookup(line, actualHit, tick)`。
3. cache 真正安装新行时调用 `cacheFill`，并传入实际 victim line 与
   demand/DVR/其他预取来源；invalidate 等非 fill 删除调用 `cacheRemove`。
4. `demandLookup` 内部只以 demand 引用更新 shadow cache，DVR fill 不更新
   shadow。因此 coverage 分母表示同一 demand 地址流、没有 DVR 时的 LRU
   cache miss 数，而不是当前含预取 cache 的 miss 数。

## 当前验证和边界

独立 smoke 覆盖及时命中、未完成请求导致的 late、counterfactual coverage、
请求/字节带宽以及一次可归因的 pollution miss：

```bash
clang++ -std=c++17 -Wall -Wextra -Werror \
  -I code/gem5-runahead-dev-pre/src \
  code/gem5-runahead-dev-pre/src/cpu/o3/dvr_quality.cc \
  code/gem5-runahead-dev-pre/src/cpu/o3/dvr_quality_smoke.cc \
  -o /tmp/dvr_quality_smoke
/tmp/dvr_quality_smoke
```

期望输出：`DVR_QUALITY_SMOKE_PASSED`。

该模块已加入 gem5 `SConscript`。CPU 已连接 helper 的 accepted issue 与
response completion，并导出严格的 `dvrQualityIssuedBytes`、
`dvrQualityCompletedBytes`；提交时还记录
`dvrQualityDemandAddressesObserved`，其名称明确说明它不是 cache hit 数。

cache 侧真实事件出口现已实现，但 CPU listener 尚未绑定到指定 L1D，所以
完整 workload 的严格 accuracy/coverage/timeliness/pollution 数字仍不可
报告。CPU 侧旧 proxy 可以用于 debug，不得在最终实验表中改名为严格指标。

新增的低侵入 cache provenance/event 接口如下：

- `Request::DVR_PREFETCH`：区分 DVR helper 与普通硬件预取；DVR source
  `ReadReq` 和 dependent `SoftPFReq` 都携带该位；
- `CacheBlk::_dvrPrefetched`：记录 DVR fill 来源，并在 demand 使用后继续
  保留到 replacement/invalidate；是否已用由 tracker 单独记录，避免删除
  时丢失 provenance；
- `BaseCache::DVRQualityEvent` / probe `"DVR Quality"`：
  - `DemandLookup` 来自 `recvTimingReq()` 的真实 `access()` 结果；
  - `Fill` 来自 `handleFill()` 成功且不是临时块之后，携带 replacement
    policy 真正选择的全部 victim；
  - `Remove` 在 `invalidateBlock()` 清除 tag/provenance 之前发出；
  - origin 为 `Demand`、`DVR` 或 `OtherPrefetch`，不会把普通预取 victim
    错算成 demand pollution。
- `mem/cache/dvr_quality_event.hh` 是不依赖 O3 的公共事件类型；
  `dvr_quality_event_smoke.cc` 独立验证三态 origin 和多 victim 载荷。

下一步只需实现一个 `ProbeListenerObject`，在 Python 配置中明确绑定
`system.cpu.dcache` 的 `"DVR Quality"` probe，并把事件转发给
`DVRQualityTracker(setCount, ways, lineBytes)`。必须绑定指定 L1D，不能
监听所有 cache，否则 L2/L3 会重复记账；物理 block address 必须用
`lineBytes` 归一化后再计算 shadow set index。

事件出口对应的真实路径为：

- demand tag lookup：`line` 与 `actualHit`；
- fill：`line`、`Demand/DVR origin`、实际 victim line 及 victim origin；
- coherence invalidate 或非 fill replacement：被删除 line 及 origin。

独立 tracker smoke 还验证：accuracy 和 coverage 使用不同分子。一个及时
有用的 DVR 行若 demand-only shadow 本来会命中，会提高 accuracy，但不会
虚增 coverage。
