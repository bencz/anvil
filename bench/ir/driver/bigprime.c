#include "bench.h"

void kntt(int32_t *t, int32_t n, int32_t s, int32_t d, int32_t h);

enum { M = 1811939329 };
static int32_t t[BENCH_PICK(1 << 21, 1 << 11)] = { 2 };

/* A big number in decimal by transforms modulo the prime M (squarings of
 * the digit vector, the carries in between); prints a checksum of the
 * digits and their count. */
int main(void)
{
    int32_t n = 1, e = BENCH_PICK(1257787, 1279), s, a, u = 1;
    int32_t *p = t;
    while (e /= 2) {
        n *= 2;
        u = (int32_t)(u * 1LL * (M + 1) / 2 % M);
        for (s = n; s /= 2;)
            kntt(t, n, s, 136, 0);
        for (p = t; p < t + n; p++)
            *p = (int32_t)(*p * 1LL * *p % M * u % M);
        for (s = 1; s < n; s *= 2)
            kntt(t, n, s, 839354248, 1);
        for (a = 0, p = t; p < t + n;) {
            a += *p << (e & 1);
            *p++ = a % 10;
            a /= 10;
        }
    }
    while (!*--p)
        ;
    uint32_t h = 0, digits = 0;
    for (t[0]--; p >= t; p--, digits++)
        h = h * 31 + (uint32_t)*p;
    printf("bigprime %u %u\n", h, digits);
    return 0;
}
