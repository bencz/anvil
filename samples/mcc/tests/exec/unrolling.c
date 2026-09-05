#include <stdio.h>

static volatile unsigned observations;

static unsigned forward(unsigned seed)
{
    unsigned a = seed;
    unsigned b = 1;
    for (unsigned index = 0; index < 8; index++)
    {
        unsigned next = a + b;
        observations = a;
        a = b;
        b = next + observations;
    }

    return a + b;
}

static unsigned backward(unsigned seed)
{
    for (int index = 5; index > 0; index--)
    {
        observations = seed;
        seed = (seed ^ (unsigned)index) + observations;
    }

    return seed;
}

static unsigned wrapped(unsigned seed)
{
    for (unsigned index = 4294967293u; index != 2; index++)
        seed += index;

    for (int index = -3; index <= 2; index++)
        seed ^= (unsigned)index;

    for (unsigned index = 0; index < 0; index++)
        observations = 999;

    return seed;
}

int main(void)
{
    unsigned total = 0;
    for (unsigned seed = 0; seed < 25; seed++)
        total += forward(seed) + backward(seed) + wrapped(seed);

    printf("%u %u\n", total, observations);
    return 0;
}
