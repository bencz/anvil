/* Loops counted by narrow integers, which C promotes to int and truncates
 * back (i += 4 on a uint16_t): the compiler may unroll them completely, and
 * the counter's and the header's values read after the loop must be those
 * of the last test. */
#include <stdint.h>
#include <stdio.h>

static const uint16_t table[16] = { 3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9, 7, 9, 3 };

static uint16_t nibbles(uint16_t a)
{
    uint16_t r = 0, i;
    for (i = 0; i < 16; i += 4) {
        r = r / 16 + table[a % 16] * 4096;
        a /= 16;
    }
    return r;
}

static int signed_steps(void)
{
    int sum = 0;
    int8_t i;
    for (i = -8; i < 8; i += 3)
        sum += i * i;
    return sum * 100 + i;
}

static unsigned wrapping(void)
{
    unsigned sum = 0;
    uint8_t i = 64;
    do {
        sum = sum * 7 + i;
        i += 64;
    } while (i != 64);
    return sum;
}

static int after_loop(void)
{
    uint16_t i;
    int last = 0;
    for (i = 2; i < 20; i += 5)
        last += i;
    return last * 1000 + i;
}

int main(void)
{
    unsigned total = 0;
    for (unsigned a = 0; a < 70000; a += 7)
        total = total * 31 + nibbles((uint16_t)a);
    printf("%u %d %u %d\n", total, signed_steps(), wrapping(), after_loop());
    return 0;
}
