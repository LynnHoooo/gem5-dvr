#include <stdint.h>

/*
 * Keep one load PC on one strictly sequential source-address stream.  An
 * earlier version alternated two source arrays between repetitions; those
 * large base-address jumps repeatedly destroyed the stride detector's
 * confidence and could leave Discovery with no committed start.
 *
 * The source values alternate parity while changing within each parity class.
 * This gives one stable trigger and two data-dependent FLR PCs:
 *
 *   indices[i] -> even_payload[indices[i]]
 *              -> odd_payload[indices[i]]
 *
 * Each FLR also sees at least two distinct values, so both affine relations
 * can train rather than merely being observed once.
 */
enum { Elements = 4096, Repetitions = 6, Mask = Elements - 1 };
static volatile uint64_t indices[Elements];
static volatile uint64_t even_payload[Elements];
static volatile uint64_t odd_payload[Elements];
static volatile uint64_t sink;

/* Prevent GCC from replacing the conditional branch with a select. */
__attribute__((optimize("no-if-conversion")))
void
_start(void)
{
    for (uint64_t i = 0; i < Elements; ++i) {
        indices[i] = (i * 17) & Mask;
        even_payload[i] = i * 3 + 1;
        odd_payload[i] = i * 5 + 7;
    }

    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Elements; ++i) {
            const uint64_t index = indices[i];
            uint64_t value;
            if (index & 1)
                value = odd_payload[index];
            else
                value = even_payload[index];

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
