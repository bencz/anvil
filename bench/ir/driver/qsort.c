#include "bench.h"

void kqsort(int32_t *a, int32_t lo, int32_t hi);

#define COUNT BENCH_PICK(400000, 6000)
static int32_t data[COUNT];

int main(void)
{
    uint32_t seed = 12345;
    long check = 0;
    for (int rep = 0; rep < BENCH_PICK(8, 1); rep++) {
        for (int i = 0; i < COUNT; i++) {
            seed = seed * 1103515245u + 12345u;
            data[i] = (int32_t)(seed >> 8);
        }
        kqsort(data, 0, COUNT - 1);
        for (int i = 1; i < COUNT; i++)
            if (data[i - 1] > data[i])
                return 1;
        check += data[COUNT / 2];
    }
    printf("qsort %ld\n", check);
    return 0;
}
