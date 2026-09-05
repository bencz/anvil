#include <stdio.h>

static int branch_loop(int limit)
{
    int value = 3;
    int sum = 0;
    if (limit & 1)
        value = 7;

    for (int index = 0; index < limit; index++)
    {
        if (index % 3 == 0)
            value += index;
        else
            value -= 2;

        sum += value;
    }

    return sum + value;
}

static int irreducible(int count)
{
    int value = 5;
    if (count & 1)
        goto second;

first:
    value += 3;
    if (--count <= 0)
        return value;

second:
    value *= 2;
    if (--count > 0)
        goto first;

    return value;
}

static int switch_join(int selector)
{
    int value = 11;
    switch (selector)
    {
        case 0:
        case 2:
            value = 4;
            break;
        case 3:
            value = 8;
            break;
        default:
            value += selector;
            break;
    }

    return value;
}

static int zero_trip(int count, int divisor)
{
    int sum = 0;
    for (int index = 0; index < count; index++)
    {
        sum += 23 / divisor;
        sum += count * 7;
    }

    return sum;
}

int main(void)
{
    for (int index = 1; index < 16; index++)
        printf("%d %d %d\n", branch_loop(index), irreducible(index), switch_join(index));

    volatile int observed = 1;
    int ordinary = 9;
    int *address = &ordinary;
    observed = 2;
    *address += observed;
    printf("%d %d\n", ordinary, observed);
    printf("%d %d\n", zero_trip(0, 0), zero_trip(4, 2));
    return 0;
}
