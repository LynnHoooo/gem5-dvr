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
// Keep enough post-training iterations for the cache-complete alternate
// template to be launched and consume source responses before program exit.
enum { Elements = 4096, Repetitions = 20, Total = Elements * Repetitions,
       Mask = Elements - 1 };
static volatile uint64_t indices[Total];
static volatile uint64_t payload[Elements];
static volatile uint64_t sink;

/* Prevent GCC from replacing the conditional branch with a select. */
__attribute__((optimize("no-if-conversion")))
void
_start(void)
{
    for (uint64_t i = 0; i < Total; ++i) {
        indices[i] = (i * 17) & Mask;
    }
    for (uint64_t i = 0; i < Elements; ++i) {
        payload[i] = i * 7 + 11;
    }

    for (uint64_t i = 0; i < Total; ++i) {
            const uint64_t index = indices[i];
            /*
             * Keep the dependent load inside each arm.  A valid alternate
             * suffix must therefore calculate and issue its own payload
             * target before both arms meet at the store below.  This makes
             * the microbenchmark a data-path gate, rather than merely a
             * branch-mask/reconvergence observation.
             */
            uint64_t value;
            if (index & 1)
                value = payload[(index + 3) & Mask];
            else
                value = payload[(index + 5) & Mask];

            /*
             * A fixed-address volatile store keeps the selected load live
             * without creating a long reduction dependence in the slice.
             */
            sink = value;
    }

    register long a0 asm("a0") = 0;
    register long a7 asm("a7") = 93;
    asm volatile("ecall" : : "r"(a0), "r"(a7) : "memory");
    __builtin_unreachable();
}
