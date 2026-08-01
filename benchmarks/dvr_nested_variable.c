#include <stdint.h>

enum {
    Outer = 256,
    MaxInner = 64,
    Repetitions = 32,
    Elements = Outer * MaxInner,
    Mask = Elements - 1
};

static volatile uint64_t outer_source[Outer];
static volatile uint64_t outer_payload[Outer];
static volatile uint64_t inner_source[Elements];
static volatile uint64_t payload[Elements];
static volatile uint64_t sink;

void
_start(void)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Outer; ++i) {
        outer_source[i] = i;
        outer_payload[i] = i * 5 + 7;
    }
    for (uint64_t i = 0; i < Elements; ++i) {
        inner_source[i] = (i * 17) & Mask;
        payload[i] = i * 3 + 1;
    }

    /* Alternate committed invocations between 16 and 32 inner lanes. */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Outer; ++i) {
            const uint64_t outer_index = outer_source[i];
            const uint64_t limit = ((outer_index + r) & 1) ? 32 : 16;
            const uint64_t base = i * MaxInner;
            sum += outer_payload[outer_index];
            for (uint64_t j = 0; j < limit; ++j) {
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
