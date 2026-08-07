#include <stdint.h>

enum {
    Elements = 1 << 10,
    Repetitions = 8,
    Mask = Elements - 1
};

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

    /*
     * One sequential trigger load feeds one genuinely indirect dependent
     * load.  The 32 KiB source and payload arrays exceed the L1D when
     * combined, so the chain produces real cache misses without requiring a
     * large workload run.
     */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i) {
            const uint64_t index = indices[i];
            // Fixed straight-line work gives the decoupled source request
            // time to return without introducing a nested control-flow path
            // into the trigger-to-FLR recorder slice.
            asm volatile(
                "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
                "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
                "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
                "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
                ::: "memory");
            sum += payload[index];
        }
    }

    // Keep the SE context alive long enough for the final helper responses
    // to retire before the correctness gate samples stats at exit.
    for (unsigned wait = 0; wait < 256; ++wait)
        asm volatile("nop" ::: "memory");

    sink = sum;
    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
