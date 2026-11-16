/* Shifts whose count is wider than the shifted value (the count's type does
 * not convert the value, and the result has the value's promoted type): an
 * int shifted by long long counts, constant and variable, left, arithmetic
 * right and logical right. */
#include <stdio.h>

static int left(int x, long long n)
{
    return x << n;
}

static int right(int x, long long n)
{
    return x >> n;
}

static unsigned logical(unsigned x, unsigned long long n)
{
    return x >> n;
}

int main(void)
{
    volatile long long three = 3;
    int sum = 0;
    for (long long n = 0; n < 31; n += 5) {
        sum = sum * 3 + left(7, n);
        sum = sum * 3 + right(-1000000, n);
        sum = sum * 3 + (int)logical(0xF0F0F0F0u, (unsigned long long)n);
    }
    printf("%d %d %u %d %d\n", 5 << 3LL, -96 >> three, 0x80000000u >> 31ULL, (int)sizeof(5 << 3LL), sum);
    return 0;
}
