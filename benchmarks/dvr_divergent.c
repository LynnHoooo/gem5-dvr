#include <stdint.h>

/*
 * Keep one load PC on one strictly sequential source-address stream.  An
 * earlier version alternated two source arrays between repetitions; those
 * large base-address jumps repeatedly destroyed the stride detector's
 * confidence and could leave Discovery with no committed start.
 *
 * The branch changes the index, then both paths join before the dependent
 * load.  This is the minimal single-entry/single-exit shape needed to train
 * an alternate path and resume at one real reconvergence/FLR PC.
 */
enum { Elements = 4096, Repetitions = 6, Mask = Elements - 1 };
static volatile uint64_t indices[Elements];
static volatile uint64_t payload[Elements];
static volatile uint64_t sink;

/* Prevent GCC from replacing the conditional branch with a select. */
__attribute__((optimize("no-if-conversion")))
void
_start(void)
{
    for (uint64_t i = 0; i < Elements; ++i) {
        indices[i] = (i * 17) & Mask;
        payload[i] = i * 7 + 11;
    }

    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i) {
            const uint64_t index = indices[i];
            uint64_t selected = index;
            if (index & 1)
                selected = (index + 3) & Mask;
            else
                selected = (index + 5) & Mask;
            uint64_t value = payload[selected];

            /*
             * A fixed-address volatile store keeps the selected load live
             * without creating a long reduction dependence in the slice.
             */
            sink = value;
        }
    }

    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
