/* Hand-written quicksort over a pseudo-random array: compares, swaps,
 * recursion, pointer arithmetic. */
#include <stdio.h>

#define COUNT 2000000
static int data[COUNT];

static void sort(int *lo, int *hi)
{
    while (hi - lo > 16) {
        int pivot = lo[(hi - lo) / 2];
        int *i = lo, *j = hi - 1;
        while (i <= j) {
            while (*i < pivot) i++;
            while (*j > pivot) j--;
            if (i <= j) {
                int t = *i; *i = *j; *j = t;
                i++; j--;
            }
        }
        if (j - lo < hi - i) {
            sort(lo, j + 1);
            lo = i;
        } else {
            sort(i, hi);
            hi = j + 1;
        }
    }
    for (int *p = lo + 1; p < hi; p++) {
        int v = *p, *q = p;
        while (q > lo && q[-1] > v) { *q = q[-1]; q--; }
        *q = v;
    }
}

int main(void)
{
    unsigned state = 12345u;
    for (int i = 0; i < COUNT; i++) {
        state = state * 1103515245u + 12345u;
        data[i] = (int)(state >> 1);
    }
    sort(data, data + COUNT);
    long check = 0;
    for (int i = 0; i < COUNT; i += 997)
        check += data[i] % 1000;
    for (int i = 1; i < COUNT; i++)
        if (data[i - 1] > data[i]) { printf("unsorted\n"); return 1; }
    printf("qsort %ld\n", check);
    return 0;
}
