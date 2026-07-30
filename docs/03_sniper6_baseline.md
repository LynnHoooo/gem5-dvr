# Sniper 6.0 Baseline

## 版本锁定

使用 Sniper 官方仓库提交：

```text
558efafeb3bb90fbf7edd4bebf04c6097e513dd9
```

该提交日期为 2014-06-02，提交信息为 `Version 6.0`。

前端使用 Pin 2.14 revision 71313。Sniper 6.0 要求 Pin revision 不低于 61206，原始 Pin 2.13.65163 已从 Intel 下载站移除。

## Table 1 映射

配置文件：`configs/sniper/dvr_table1_baseline.cfg`

| 论文参数 | Sniper 6.0 配置 | 状态 |
|---|---|---|
| 4 GHz | `perf_model/core/frequency=4.0` | 精确 |
| OoO | `type=rob`, `in_order=false` | 精确 |
| ROB 350 | `window_size=350` | 精确 |
| Issue queue 128 | `rs_entries=128` | 精确 |
| Load queue 128 | `outstanding_loads=128` | 精确 |
| Store queue 72 | `outstanding_stores=72` | 精确 |
| 5-wide | `dispatch_width=5`, `commit_width=5` | 可配置部分精确 |
| 15-stage front end | `mispredict_penalty=15` | 近似 |
| 8 KB TAGE-SC-L | `pentium_m` | 不可精确，公开版缺少 TAGE-SC-L |
| 标量 FU 数量 | Nehalem port model | 不可精确，需要作者扩展 |
| 向量 FU/寄存器 | 无 | 不可配置，需要 DVR 扩展 |
| L1I | 32 KB/4-way/2 cycles | 精确 |
| L1D | 32 KB/8-way/4 cycles/24 MSHRs | 精确 |
| L1D stride prefetcher | simple, 16 flows | 结构近似 |
| L2 | 256 KB/8-way/8 cycles | 精确 |
| L3 | 8 MB/16-way/30 cycles | 精确 |
| DRAM latency | 50 ns | 精确 |
| DRAM bandwidth | 51.2 GB/s | 单控制器等效配置 |

## 复现边界

该配置建立的是可公开重建的 Sniper 6.0 baseline。论文使用的 TAGE-SC-L、Ice Lake 风格执行资源、向量资源与 DVR 机制不在公开 Sniper 6.0 中，因此需要作者补丁才能达到 bit-for-bit 的原始实验环境。
