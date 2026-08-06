#include <stdint.h>

enum { Elements = 4096, Repetitions = 12, Mask = Elements - 1 };
static volatile uint64_t indices[Elements];
static volatile uint64_t payload[Elements];
static volatile uint8_t stop[Elements];
static volatile uint64_t sink;

__attribute__((noinline)) static uint64_t
explicit_lcr(uint64_t limit)
{
    uint64_t sum = 0;
    uint64_t i = 0;
    for (;;) {
        uint64_t predicate;
        asm volatile ("slt %0, %1, %2" : "=r"(predicate) : "r"(i), "r"(limit));
        if (!predicate)
            break;
        const uint64_t index = indices[i];
        sum += payload[index];
        ++i;
    }
    return sum;
}

__attribute__((noinline)) static uint64_t
fused_lcr(uint64_t limit)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < limit; ++i)
        sum += payload[indices[i]];
    return sum;
}

__attribute__((noinline)) static uint64_t
fallback_and_squash(void)
{
    uint64_t sum = 0;
    uint64_t i = 0;
    while (i < Elements) {
        const uint64_t index = indices[i];
        sum += payload[index];
        // The data-dependent early exit is deliberately not an induction
        // compare-to-branch pair, so this path exercises the 128-lane fallback.
        if (stop[index])
            i = (i + 17) & Mask;
        else
            ++i;
    }
    return sum;
}

void
_start(void)
{
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Elements; ++i) {
        indices[i] = (i * 17) & Mask;
        payload[i] = i * 13 + 7;
        stop[i] = ((i * 1103515245u) >> 11) & 1;
    }
    for (unsigned r = 0; r < Repetitions; ++r) {
        sum += explicit_lcr(Elements);
        sum += fused_lcr(Elements);
        sum += fallback_and_squash();
    }
    sink = sum;
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
