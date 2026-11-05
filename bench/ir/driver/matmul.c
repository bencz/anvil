#include "bench.h"

void kmatmul(double *c, const double *a, const double *b, int32_t n);

#define N BENCH_PICK(256, 40)
static double a[N * N], b[N * N], c[N * N];

int main(void)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i * N + j] = (double)((i * 7 + j * 3) % 17) / 16.0;
            b[i * N + j] = (double)((i * 5 + j * 11) % 13) / 12.0;
        }
    for (int rep = 0; rep < BENCH_PICK(6, 1); rep++)
        kmatmul(c, a, b, N);
    double sum = 0.0;
    for (int i = 0; i < N * N; i++)
        sum += c[i] * (double)(i & 7);
    printf("matmul %lld\n", (long long)(sum * 1024.0));
    return 0;
}
