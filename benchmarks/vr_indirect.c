/*
 * VR (Vector Runahead, ISCA 2020) 复现 -- Stage 1 微基准。
 *
 * 单个循环内包含两条 load，正好对应论文的验证场景：
 *
 *   stride_val = values[i];            // (A) 稳定 +8 字节跨步 load -> VR 触发点
 *   sum += deps[foo(stride_val)];      // (B) 地址依赖 (A) 的 load -> 依赖链叶子
 *
 * 运行期行为：
 *   - (A) 以固定 +8 步幅反复命中同一 PC：在 PRE 助线程重放期间，
 *     步幅检测器累计置信度，达到 3 后进入 VR，把 (A) 向量化为 N 通道 gather；
 *   - (B) 的地址由 (A) 的返回值经移位/掩码计算得到：VR 回放已记录链后，
 *     为 (B) 生成依赖 gather 预取（对应 LoadAddress uop）。
 *
 * deps 工作集 (128 KB) 大于 L1，且索引经奇数乘子散列，保证循环内持续出现
 * L1 缺失以触发 PRE 进入，同时 (A) 的稳定步幅让 VR 在 PRE 期间被训练出来。
 *
 * 退出用 Linux RISC-V exit(0) 系统调用，避免依赖宿主 glibc 启动。
 */

#include <stdint.h>

enum {
    StrideElements = 4096,   /* (A) 流长度：4096 * 8 B = 32 KB */
    DepElements = 16384,     /* (B) 工作集：16384 * 8 B = 128 KB */
    DepMask = DepElements - 1,
    Repetitions = 16
};

static volatile uint64_t values[StrideElements];
static volatile uint64_t deps[DepElements];
static volatile uint64_t sink;

void
_start(void)
{
    uint64_t sum = 0;

    for (uint64_t i = 0; i < StrideElements; ++i)
        values[i] = i * 3;          /* 奇数乘子：散列到 deps 的跨度 */
    for (uint64_t i = 0; i < DepElements; ++i)
        deps[i] = i * 7 + 1;

    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < StrideElements; ++i) {
            const uint64_t stride_val = values[i];
            sum += deps[(stride_val * 3) & DepMask];
        }
    }

    sink = sum;

    /* Linux RISC-V exit(0)，避免宿主 glibc 启动依赖。 */
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
