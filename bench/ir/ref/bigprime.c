#include <stdint.h>

#define M 1811939329

/* One pass of the transform over t[0..n): blocks of 2s, butterflies of
 * p[0] and p[s] with the powers c of d (h: the inverse direction). */
void kntt(int32_t *t, int32_t n, int32_t s, int32_t d, int32_t h)
{
    for (int32_t i = s; i < 1 << 25; i *= 2)
        d = (int32_t)(d * 1LL * d % M);
    for (int32_t k = 0; k < n; k += 2 * s) {
        int32_t c = 1;
        for (int32_t j = 0; j < s; j++) {
            int32_t *p = t + k + j;
            int32_t a = (int32_t)(p[s] * (h ? c : 1LL) % M);
            p[s] = (int32_t)(((uint32_t)M + (uint32_t)*p - (uint32_t)a) * (h ? 1LL : c) % M);
            *p = (int32_t)(((uint32_t)a + (uint32_t)*p) % (uint32_t)M);
            c = (int32_t)(c * 1LL * d % M);
        }
    }
}
