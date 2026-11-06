/* switch on 64-bit selectors: cases that differ only in the high word, a
 * negative case, a chain of few cases and a tree of many, signed and
 * unsigned, as targets with 32-bit registers compare word pairs. */
#include <stdio.h>

static int few(long long x)
{
    switch (x) {
    case 5:
        return 1;
    case 5LL + (1LL << 32):
        return 2;
    case -1:
        return 3;
    default:
        return 0;
    }
}

static int many(unsigned long long x)
{
    switch (x) {
    case 0:
        return 10;
    case 1:
        return 11;
    case 2:
        return 12;
    case 1ULL << 40:
        return 13;
    case 0xFFFFFFFFULL:
        return 14;
    case 0x100000000ULL:
        return 15;
    case 0xFFFFFFFFFFFFFFFFULL:
        return 16;
    default:
        return 17;
    }
}

int main(void)
{
    long long inputs[] = {5, 5LL + (1LL << 32), -1, 0, 4294967295LL, 1LL << 40, 4294967296LL, 2, 77};
    int sum = 0;
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        sum = sum * 7 + few(inputs[i]);
        sum = sum * 3 + many((unsigned long long)inputs[i]);
    }
    printf("%d %d %d %d\n", few(5LL + (1LL << 32)), many(0x100000000ULL), many(0xFFFFFFFFFFFFFFFFULL), sum);
    return 0;
}
