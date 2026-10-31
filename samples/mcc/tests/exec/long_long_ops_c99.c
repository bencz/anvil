/* 64-bit integer arithmetic, exercised at the edges where a 32-bit target
 * splits values into word pairs: carries and borrows across bit 31,
 * shifts by 0/31/32/33/63, signed and unsigned division, comparisons that
 * differ only in the low word, conversions and values flowing through
 * branches and loops. */
#include <stdio.h>

typedef long long i64;
typedef unsigned long long u64;

static const i64 values[] = {
    0, 1, -1, 2, -2, 0x7fffffffLL, 0x80000000LL, 0xffffffffLL, 0x100000000LL,
    -0x80000000LL, -0x100000001LL, 0x123456789abcdefLL, -0x123456789abcdefLL,
    0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1, 1000000007LL, -999999937LL,
};
#define COUNT (int)(sizeof(values) / sizeof(values[0]))

static u64 mix(u64 h, u64 v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

static int shift_amounts[] = { 0, 1, 7, 31, 32, 33, 47, 63 };

static u64 arithmetic(void)
{
    u64 h = 0;
    for (int i = 0; i < COUNT; i++)
        for (int j = 0; j < COUNT; j++) {
            i64 a = values[i], b = values[j];
            u64 ua = (u64)a, ub = (u64)b;
            h = mix(h, ua + ub);
            h = mix(h, ua - ub);
            h = mix(h, ua * ub);
            h = mix(h, ua & ub);
            h = mix(h, ua | ub);
            h = mix(h, ua ^ ub);
            if (b != 0) {
                h = mix(h, ua / ub);
                h = mix(h, ua % ub);
                if (!(a == -0x7fffffffffffffffLL - 1 && b == -1)) {
                    h = mix(h, (u64)(a / b));
                    h = mix(h, (u64)(a % b));
                }
            }
            h = mix(h, (a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 |
                       (a == b) << 4 | (a != b) << 5 | (ua < ub) << 6 | (ua <= ub) << 7 |
                       (ua > ub) << 8 | (ua >= ub) << 9);
        }
    return h;
}

static u64 shifts(int variable_offset)
{
    u64 h = 0;
    for (int i = 0; i < COUNT; i++)
        for (int k = 0; k < (int)(sizeof(shift_amounts) / sizeof(shift_amounts[0])); k++) {
            /* variable_offset is 0 at run time; it keeps the count variable. */
            int s = shift_amounts[k] + variable_offset;
            i64 a = values[i];
            h = mix(h, (u64)a << s);
            h = mix(h, (u64)a >> s);
            h = mix(h, (u64)(a >> s));
        }
    i64 v = values[11];
    h = mix(h, (u64)v << 0);
    h = mix(h, (u64)v << 31);
    h = mix(h, (u64)v << 32);
    h = mix(h, (u64)v << 33);
    h = mix(h, (u64)v >> 63);
    h = mix(h, (u64)(v >> 40));
    h = mix(h, (u64)(-v >> 32));
    return h;
}

static u64 unary_and_conversions(void)
{
    u64 h = 0;
    for (int i = 0; i < COUNT; i++) {
        i64 a = values[i];
        h = mix(h, (u64)-a);
        h = mix(h, (u64)~a);
        h = mix(h, (u64)(int)a);
        h = mix(h, (u64)(unsigned)a);
        h = mix(h, (u64)(short)a);
        h = mix(h, (u64)(signed char)a);
        h = mix(h, (u64)(i64)(int)a);
        h = mix(h, (u64)(unsigned)(a >> 16));
        double d = (double)a;
        float f = (float)a;
        /* Scaled so every conversion back stays in range. */
        h = mix(h, (u64)(i64)(d / 3.0));
        h = mix(h, (u64)(i64)(f / 4.0f));
        h = mix(h, (u64)((double)(u64)a / 4.0));
        h = mix(h, (u64)((float)(u64)a / 4.0f));
        /* Exact integral doubles: no excess-precision dependence. */
        h = mix(h, (unsigned)(3e9 + (double)(i64)(d / 8589934592.0)));
    }
    for (int x = -3; x <= 3; x++) {
        h = mix(h, (u64)(i64)x);
        h = mix(h, (u64)(unsigned)x);
    }
    h = mix(h, (u64)(18446744073709549568.0));
    h = mix(h, (u64)(i64)-9007199254740993.0);
    return h;
}

static i64 triangle(int n)
{
    i64 sum = 0;
    for (int i = 0; i < n; i++)
        sum += (i64)i * 0x10000001LL;
    return sum;
}

static i64 pick(int c, i64 a, i64 b)
{
    return c ? a : b;
}

static i64 collatz_steps(u64 n)
{
    i64 steps = 0;
    while (n != 1) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}

int main(void)
{
    printf("arith %016llx\n", arithmetic());
    printf("shift %016llx\n", shifts(0));
    printf("conv  %016llx\n", unary_and_conversions());
    printf("tri   %lld %lld\n", triangle(1000), triangle(100000));
    printf("pick  %lld %lld\n", pick(1, 0x100000000LL, -5), pick(0, 7, -0x100000000LL));
    printf("steps %lld %lld\n", collatz_steps(27), collatz_steps(0x2fffffffULL));
    i64 big = 0x7ffffffeLL;
    big += 3;
    big *= 3;
    big -= 0x100000000LL;
    printf("chain %lld %llu\n", big, (u64)big >> 1);
    return 0;
}
