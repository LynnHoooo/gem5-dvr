#include <stdint.h>

enum {
    Elements = 1 << 12,
    Repetitions = 8,
    Mask = Elements - 1
};

static volatile uint64_t indices[Elements];
static volatile uint64_t payload[Elements];
static volatile uint64_t delay_state;
static volatile uint64_t sink;

static uint64_t
mix(uint64_t x)
{
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * UINT64_C(2685821657736338717);
}

void
_start(void)
{
    uint64_t state = 1;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < Elements; ++i) {
        state = mix(state + i);
        indices[i] = state & Mask;
        payload[i] = i * 3 + 1;
    }

    /*
     * One sequential trigger load feeds one genuinely indirect dependent
     * load.  The 32 KiB source and payload arrays exceed the L1D when
     * combined, so the chain produces real cache misses without requiring a
     * large workload run.
     */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i) {
            const uint64_t index = indices[i];
            /*
             * Leave a real window between the source value and the
             * dependent demand.  This is still a single A[B[i]] chain, but
             * it makes the decoupled helper's lead time measurable rather
             * than forcing every implementation to race the same-cycle
             * demand load.
             */
            for (unsigned k = 0; k < 8; ++k)
                delay_state = mix(delay_state + k);
            sum += payload[index];
        }
    }

    sink = sum;
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
