#include <stdint.h>

enum {
    Outer = 256,
    Inner = 256,
    Repetitions = 16,
    Elements = Outer * Inner,
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

    /*
     * outer_source is the root striding trigger.  While its discovery is
     * active, inner_source becomes a second committed striding trigger.  Its
     * value feeds payload[index], giving the child an actual trigger-to-FLR
     * slice and a recurring inner-loop branch from which to infer lanes.
     */
    for (unsigned r = 0; r < Repetitions; ++r) {
        for (uint64_t i = 0; i < Outer; ++i) {
            const uint64_t outer_index = outer_source[i];
            sum += outer_payload[outer_index];
            const uint64_t base = i * Inner;
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
