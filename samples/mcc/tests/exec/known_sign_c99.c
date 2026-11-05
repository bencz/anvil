/* Signed operations of values whose sign bit is known zero (masked,
 * zero-extended or shifted right) run as their unsigned forms; the results
 * must stay those of the signed operations, and values of unknown sign keep
 * the signed ones. */
#include <stdio.h>

static int masked(int x)
{
    int m = x & 0x7fff;
    short narrow = (short)m;
    return (int)narrow + (m >> 3) + m / 8 + m % 16 + (m & 0xffff);
}

static long widened(unsigned char c, int k)
{
    int v = c;
    return (long)v + v / 4 + v % 4 + (v >> 1) + k / 4 + k % 4 + (k >> 1);
}

static int shifted(unsigned u)
{
    int v = (int)(u >> 1);
    return v / 32 + v % 32 + (v >> 5);
}

int main(void)
{
    static const int inputs[] = { 0, 1, 7, 8, 15, 16, -1, -8, -17, 0x7fff, 0x8000, -0x8000, 123456, -123456, 0x7fffffff, -0x7fffffff - 1 };
    long total = 0;
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        int x = inputs[i];
        printf("%d: %d %ld %d\n", x, masked(x), widened((unsigned char)x, x), shifted((unsigned)x));
        total += masked(x) + widened((unsigned char)x, x) + shifted((unsigned)x);
    }
    printf("total %ld\n", total);
    return 0;
}
