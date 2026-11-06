#include <stdint.h>

void kcells(int32_t *t, int32_t n)
{
    for (int32_t k = 1; k < n; k++)
        while (t[k]) {
            t[k - 1] += 3;
            t[k]--;
        }
}
