#include <stdio.h>

static int adjust(int value)
{
    switch (value & 3)
    {
        case 0:
            return value + 7;
        case 1:
            return value - 3;
        default:
            return value * 2;
    }
}

static int accumulate(int count, int seed)
{
    int sum = seed;
    for (int index = 0; index < count; index++)
        sum += adjust(index + seed);

    return sum;
}

static void publish(volatile int *destination, int value)
{
    *destination = adjust(value);
}

static int recursive(int value)
{
    if (value < 2)
        return value;

    return recursive(value - 1) + recursive(value - 2);
}

int main(void)
{
    volatile int result = 0;
    int total = 0;
    for (int seed = -7; seed < 15; seed++)
    {
        total += accumulate(13, seed);
        publish(&result, total);
        total += result % 17;
    }

    printf("%d %d %d\n", total, result, recursive(12));
    return 0;
}
