#include <stdint.h>

enum { Elements = 16384, Repetitions = 8 };
static volatile uint64_t values[Elements];
static volatile uint64_t sink;

void
_start(void)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Elements; ++i)
        values[i] = i * 3 + 1;

    /* A stable +8-byte load stream used to validate DVR's RPT. */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i)
            sum += values[i];
    }

    sink = sum;

    /* Linux RISC-V exit(0), avoiding a host glibc startup dependency. */
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
