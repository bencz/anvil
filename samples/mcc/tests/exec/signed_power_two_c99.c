#include <stdio.h>

static int quotient(int value)
{
    return value / 8;
}

static int remainder(int value)
{
    return value % 8;
}

static long long wide_quotient(long long value)
{
    return value / 1024;
}

static long long wide_remainder(long long value)
{
    return value % 1024;
}

int main(void)
{
    int values[] = { -2147483647 - 1, -2147483647, -129, -128, -127, -9, -8, -7, -1, 0, 1, 7, 8, 9, 2147483647 };
    long long wide[] = { -9223372036854775807LL - 1, -9223372036854775807LL, -1025, -1024, -1023, -1, 0, 1, 1023, 1024, 1025, 9223372036854775807LL };
    volatile int divisor = 8;
    volatile long long wide_divisor = 1024;
    unsigned i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    {
        int value = values[i];
        if (quotient(value) != value / divisor || remainder(value) != value % divisor)
            return 1;
    }

    for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
    {
        long long value = wide[i];
        if (wide_quotient(value) != value / wide_divisor || wide_remainder(value) != value % wide_divisor)
            return 2;
    }

    puts("signed division and remainder passed");
    return 0;
}
