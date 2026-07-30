#include <stdint.h>

enum { Elements = 16384, Repetitions = 8, Mask = Elements - 1 };
static volatile uint64_t indices[Elements];
static volatile uint64_t payload[Elements];
static volatile uint64_t sink;

void
_start(void)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Elements; ++i) {
        indices[i] = (i * 17) & Mask;
        payload[i] = i * 3 + 1;
    }

    /* The payload load address transitively depends on the striding load. */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i) {
            const uint64_t index = indices[i];
            sum += payload[index];
        }
    }

    sink = sum;
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
