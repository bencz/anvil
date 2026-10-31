#include <stdio.h>

/* Sums of narrow elements into wider accumulators: at O3 the vector loop
 * accumulates the lanes into wider ones (psadbw, pmaddwd, uadalp,
 * vsum4ubs...) and must give the scalar loop's result for every length. */
typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef short i16;

static u8 a8[1003];
static i8 s8[1003];
static u16 a16[1003];
static i16 s16[1003];

static int sum_u8(const u8 *p, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static int sum_i8(const i8 *p, int n)
{
    int s = 3;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static unsigned sum_u16(const u16 *p, int n)
{
    unsigned s = 7;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static int sum_i16(const i16 *p, int n)
{
    int s = -5;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static long long sum_u8_long(const u8 *p, int n)
{
    long long s = 1;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

static long long sum_i16_long(const i16 *p, int n)
{
    long long s = -1;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}

int main(void)
{
    unsigned x = 12345;
    for (int i = 0; i < 1003; i++) {
        x = x * 1103515245u + 12345u;
        a8[i] = (u8)(x >> 16);
        s8[i] = (i8)(x >> 8);
        a16[i] = (u16)(x >> 3);
        s16[i] = (i16)(x >> 5);
    }
    for (int n = 0; n < 1003; n += 61)
        printf("%d %d %d %u %d %lld %lld\n", n, sum_u8(a8, n), sum_i8(s8, n), sum_u16(a16, n), sum_i16(s16, n), sum_u8_long(a8, n),
               sum_i16_long(s16, n));
    /* Every byte at its maximum: the lanes' sums must not saturate. */
    for (int i = 0; i < 1003; i++) {
        a8[i] = 255;
        s8[i] = -128;
        a16[i] = 65535;
        s16[i] = -32768;
    }
    printf("%d %d %u %d %lld\n", sum_u8(a8, 1003), sum_i8(s8, 1003), sum_u16(a16, 1003), sum_i16(s16, 1003), sum_u8_long(a8, 1003));
    return 0;
}
