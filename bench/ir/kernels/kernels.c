/* The kernels anvil-bench-gen can build (one file each in this directory,
 * with ref/<name>.c the same algorithm in C and driver/<name>.c the
 * workload that runs either). */
#include "../kit.h"

bool bench_build_fib(bench_t *b);
bool bench_build_crc32(bench_t *b);
bool bench_build_sieve(bench_t *b);
bool bench_build_matmul(bench_t *b);
bool bench_build_strings(bench_t *b);
bool bench_build_qsort(bench_t *b);

const bench_kernel_t bench_kernels[] = {
    {"fib", bench_build_fib},       {"crc32", bench_build_crc32},     {"sieve", bench_build_sieve},
    {"matmul", bench_build_matmul}, {"strings", bench_build_strings}, {"qsort", bench_build_qsort},
};

const size_t bench_num_kernels = sizeof(bench_kernels) / sizeof(bench_kernels[0]);
