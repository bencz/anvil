#include <stdint.h>

void kqsort(int32_t *a, int32_t lo, int32_t hi)
{
    while (lo < hi) {
        int32_t pivot = a[lo + ((hi - lo) >> 1)], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot)
                i++;
            while (a[j] > pivot)
                j--;
            if (i <= j) {
                int32_t t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        kqsort(a, lo, j);
        lo = i;
    }
}
