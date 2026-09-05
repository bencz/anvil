#include "aggregate_abi.h"

struct Byte change_byte(struct Byte value)
{
    value.value += 3;
    return value;
}

struct Word change_word(struct Word value)
{
    value.value += 1234;
    return value;
}

struct Odd change_odd(int prefix, struct Odd value, double scale, int tail)
{
    value.bytes[0] += prefix;
    value.bytes[1] += (int)scale;
    value.bytes[2] += tail;
    return value;
}

struct Small change_small(struct Small value)
{
    value.value += 42;
    return value;
}

struct Eight change_eight(struct Eight value)
{
    int i;
    for (i = 0; i < 8; i++)
        value.bytes[i] += i + 1;

    return value;
}

struct Pair change_pair(int prefix, struct Pair value, double scale, int tail)
{
    value.left = value.left * scale + prefix;
    value.right += tail;
    return value;
}

struct Large change_large(struct Large value, int b, int c, int d, int e)
{
    value.a += b;
    value.b += c;
    value.c += d + e;
    return value;
}

union Scalar change_scalar(union Scalar value)
{
    value.real += 0.25;
    return value;
}

struct Buffer change_buffer(struct Buffer value)
{
    int i;
    for (i = 0; i < 83; i++)
        value.bytes[i] += 2;

    return value;
}
