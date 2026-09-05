#include "aggregate_abi.h"

int main(void)
{
    struct Byte byte;
    struct Word word;
    struct Odd odd;
    struct Small small;
    struct Eight eight;
    struct Pair pair;
    struct Large large;
    union Scalar scalar;
    struct Buffer buffer;
    int i;

    byte.value = 250;
    word.value = 60000;
    odd.bytes[0] = 10;
    odd.bytes[1] = 20;
    odd.bytes[2] = 30;
    small.value = -17;
    for (i = 0; i < 8; i++)
        eight.bytes[i] = i + 10;

    pair.left = 2.5;
    pair.right = -4.0;
    large.a = 10000000000LL;
    large.b = -20000000000LL;
    large.c = 30000000000LL;
    scalar.real = 1.5;
    for (i = 0; i < 83; i++)
        buffer.bytes[i] = i;

    struct Byte rb = change_byte(byte);
    struct Word rw = change_word(word);
    struct Odd ro = change_odd(3, odd, 4.0, 5);
    struct Small rs = change_small(small);
    struct Eight re = change_eight(eight);
    struct Pair rp = change_pair(3, pair, 4.0, 5);
    struct Large rl = change_large(large, 2, 3, 4, 5);
    union Scalar ru = change_scalar(scalar);

    if (rb.value != 253 || rw.value != 61234 || rs.value != 25)
        return 1;

    if (ro.bytes[0] != 13 || ro.bytes[1] != 24 || ro.bytes[2] != 35)
        return 2;

    for (i = 0; i < 8; i++)
    {
        if (re.bytes[i] != 11 + i * 2 || eight.bytes[i] != i + 10)
            return 3;
    }

    if (rp.left != 13.0 || rp.right != 1.0 || ru.real != 1.75)
        return 4;

    if (rl.a != 10000000002LL || rl.b != -19999999997LL || rl.c != 30000000009LL)
        return 5;

    if (byte.value != 250 || word.value != 60000 || small.value != -17 || odd.bytes[0] != 10)
        return 6;

    if (pair.left != 2.5 || pair.right != -4.0 || large.a != 10000000000LL || scalar.real != 1.5)
        return 7;

    struct Large (*large_function)(struct Large, int, int, int, int) = change_large;
    struct Small (*small_function)(struct Small) = change_small;
    rl = large_function(large, 6, 7, 8, 9);
    rs = small_function(small);
    if (rl.a != 10000000006LL || rl.c != 30000000017LL || rs.value != 25)
        return 8;

    struct Buffer output = change_buffer(buffer);
    for (i = 0; i < 83; i++)
    {
        if (output.bytes[i] != i + 2 || buffer.bytes[i] != i)
            return 9;
    }

    rs = change_small(change_small(small));
    if (rs.value != 67 || change_pair(3, pair, 4.0, 5).left != 13.0)
        return 10;

    rs = small.value ? small : rs;
    if (rs.value != -17)
        return 11;

    return 0;
}
