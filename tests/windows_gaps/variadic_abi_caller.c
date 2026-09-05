#include "variadic_abi.h"

int main(void)
{
    struct Small small;
    struct Large large;
    struct Odd odd;
    int pointed = 17;
    small.value = 7;
    large.a = 10;
    large.b = 20;
    large.c = 30;
    odd.bytes[0] = 1;
    odd.bytes[1] = 2;
    odd.bytes[2] = 3;

    if (variadic_mixed(6, 5, 2.5, small, large, &pointed, odd) != 100.5)
        return 1;

    if (variadic_stack(1, 2, 3, 4, 5, 10000000000LL, -20000000000LL) != -9999999985LL)
        return 2;

    struct Large result = variadic_result(3, 4.0, large, 5);
    if (result.a != 13 || result.b != 24 || result.c != 35)
        return 3;

    float single = 1.25f;
    if (variadic_float(1.0, single, 2.5, 4.0, 5.0) != 13.75)
        return 4;

    if (variadic_vla(6, 1, 2, 3, 4, 5, 6) != 21)
        return 5;

    return 0;
}
