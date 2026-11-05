#include <stdint.h>

void kmatmul(double *c, const double *a, const double *b, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
        for (int32_t k = 0; k < n; k++) {
            double aik = a[i * n + k];
            for (int32_t j = 0; j < n; j++)
                c[i * n + j] += aik * b[k * n + j];
        }
}
