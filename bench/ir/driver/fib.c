#include "bench.h"

int32_t kfib(int32_t n);

int main(void)
{
    printf("fib %d\n", kfib(BENCH_PICK(35, 20)));
    return 0;
}
