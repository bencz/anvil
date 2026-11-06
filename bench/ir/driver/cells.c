#include "bench.h"

void kcells(int32_t *t, int32_t n);

enum { COUNT = 4096 };
static int32_t tape[COUNT];

int main(void)
{
    uint32_t h = 0;
    for (int rep = 0; rep < BENCH_PICK(400, 4); rep++) {
        for (int32_t k = 0; k < COUNT; k++)
            tape[k] = (k * 7 + rep) % 61;
        kcells(tape, COUNT);
        for (int32_t k = 0; k < COUNT; k += 37)
            h = h * 31 + (uint32_t)tape[k];
    }
    printf("cells %u\n", h);
    return 0;
}
