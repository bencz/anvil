#include <stdio.h>

static int iterate(int count, int factor, int path)
{
    int index;
    int sum;
    if (path)
    {
        index = 0;
        sum = 1;
        goto header;
    }

    index = 1;
    sum = 2;
header:
    if (index >= count)
        return sum;

    sum += factor * 2;
    index++;
    goto header;
}

static int nested(int count, int factor)
{
    int sum = 0;
    for (int outer = 0; outer < count; outer++)
    {
        for (int inner = 0; inner < count; inner++)
            sum += (factor * 3) + outer;
    }

    return sum;
}

int main(void)
{
    for (int count = 0; count < 9; count++)
    {
        for (int factor = -7; factor < 8; factor++)
            printf("%d %d %d\n", iterate(count, factor, 0), iterate(count, factor, 1), nested(count, factor));
    }

    return 0;
}
