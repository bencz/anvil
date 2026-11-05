#include "bench.h"

int32_t ksieve(uint8_t *composite, int32_t limit);

#define LIMIT BENCH_PICK(4000000, 100000)
static uint8_t composite[LIMIT + 1];

int main(void)
{
    long total = 0;
    for (int rep = 0; rep < BENCH_PICK(12, 1); rep++) {
        memset(composite, 0, sizeof(composite));
        total += ksieve(composite, LIMIT);
    }
    printf("sieve %ld\n", total);
    return 0;
}
