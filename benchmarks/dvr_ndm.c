#include <stdint.h>

enum {
    Outer = 256,
    Inner = 16,
    Repetitions = 32,
    Elements = Outer * Inner,
    Mask = Elements - 1
};

static volatile uint64_t outer_source[Outer];
static volatile uint64_t inner_source[Elements];
static volatile uint64_t payload[Elements];
static volatile uint64_t sink;

void
_start(void)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Outer; ++i)
        outer_source[i] = i;
    for (uint64_t i = 0; i < Elements; ++i) {
        inner_source[i] = (i * 17) & Mask;
        payload[i] = i * 3 + 1;
    }

    /*
     * The 16-iteration inner loop is deliberately below the paper's default
     * 64-lane NDM threshold.  outer_source and inner_source provide distinct
     * committed stride candidates while the control-only NDM generation is
     * looking for an outer trigger.
     */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Outer; ++i) {
            const uint64_t outer = outer_source[i];
            const uint64_t base = outer * Inner;
            for (uint64_t j = 0; j < Inner; ++j) {
                const uint64_t index = inner_source[base + j];
                sum += payload[index];
            }
        }
    }

    sink = sum;
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
