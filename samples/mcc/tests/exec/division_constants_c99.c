#include <stdio.h>

/* Division and remainder by constants (multiply-high by the reciprocal
 * where the target has it) against every edge of each type. */
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;
typedef long long i64;

#define U32_CASE(name, d) \
    static u32 name##_q(u32 x) { return x / (d); } \
    static u32 name##_r(u32 x) { return x % (d); }
#define I32_CASE(name, d) \
    static i32 name##_q(i32 x) { return x / (d); } \
    static i32 name##_r(i32 x) { return x % (d); }
#define U64_CASE(name, d) \
    static u64 name##_q(u64 x) { return x / (d); } \
    static u64 name##_r(u64 x) { return x % (d); }
#define I64_CASE(name, d) \
    static i64 name##_q(i64 x) { return x / (d); } \
    static i64 name##_r(i64 x) { return x % (d); }

U32_CASE(u3, 3u)
U32_CASE(u7, 7u)
U32_CASE(u10, 10u)
U32_CASE(u60, 60u)
U32_CASE(u641, 641u)
U32_CASE(u1000, 1000u)
U32_CASE(ubig, 1000000007u)
U32_CASE(uhalf, 0x80000001u)
U32_CASE(umax, 0xfffffffeu)
I32_CASE(s3, 3)
I32_CASE(s7, 7)
I32_CASE(s10, 10)
I32_CASE(s100, 100)
I32_CASE(sm3, -3)
I32_CASE(sm7, -7)
I32_CASE(sm10, -10)
I32_CASE(sbig, 0x7fffffff)
I32_CASE(smin, (-0x7fffffff - 1))
U64_CASE(w3, 3ull)
U64_CASE(w7, 7ull)
U64_CASE(w10, 10ull)
U64_CASE(wbillion, 1000000000ull)
U64_CASE(wodd, 0x5555555555555555ull)
U64_CASE(wmax, 0xfffffffffffffffdull)
I64_CASE(l3, 3ll)
I64_CASE(l7, 7ll)
I64_CASE(l10, 10ll)
I64_CASE(lm7, -7ll)
I64_CASE(lbillion, 1000000000ll)
I64_CASE(lbig, 0x7fffffffffffffffll)

static u64 state = 0x9e3779b97f4a7c15ull;

static u64 next(void)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return state ^ (state >> 29);
}

static u64 mix(u64 h, u64 v)
{
    return (h ^ v) * 0x100000001b3ull;
}

#define RUN(name, type, values, count) \
    do { \
        u64 h = 1469598103934665603ull; \
        for (int i = 0; i < (count); i++) { \
            type x = (type)(values)[i]; \
            h = mix(h, (u64)name##_q(x)); \
            h = mix(h, (u64)name##_r(x)); \
        } \
        printf("%-9s %016llx\n", #name, h); \
    } while (0)

int main(void)
{
    enum { COUNT = 4096 };
    static u64 values[COUNT];
    const u64 edges[] = {0, 1, 2, 3, 6, 7, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 0x7ffffffeu, 0x7fffffffu, 0x80000000u,
                         0x80000001u, 0xfffffffeu, 0xffffffffu, 0x100000000ull, 0x7fffffffffffffffull, 0x8000000000000000ull,
                         0x8000000000000001ull, 0xfffffffffffffffeull, 0xffffffffffffffffull, (u64)-3, (u64)-7, (u64)-10,
                         (u64)-1000000000ll, 1000000006ull, 1000000007ull, 2000000014ull};
    int n = 0;
    for (unsigned e = 0; e < sizeof(edges) / sizeof(edges[0]); e++)
        values[n++] = edges[e];
    for (; n < COUNT; n++) {
        u64 v = next();
        /* Small, word-sized and full-width magnitudes. */
        values[n] = n % 3 == 0 ? v % 100000 : n % 3 == 1 ? (u32)v : v;
    }

    RUN(u3, u32, values, COUNT);
    RUN(u7, u32, values, COUNT);
    RUN(u10, u32, values, COUNT);
    RUN(u60, u32, values, COUNT);
    RUN(u641, u32, values, COUNT);
    RUN(u1000, u32, values, COUNT);
    RUN(ubig, u32, values, COUNT);
    RUN(uhalf, u32, values, COUNT);
    RUN(umax, u32, values, COUNT);
    RUN(s3, i32, values, COUNT);
    RUN(s7, i32, values, COUNT);
    RUN(s10, i32, values, COUNT);
    RUN(s100, i32, values, COUNT);
    RUN(sm3, i32, values, COUNT);
    RUN(sm7, i32, values, COUNT);
    RUN(sm10, i32, values, COUNT);
    RUN(sbig, i32, values, COUNT);
    RUN(smin, i32, values, COUNT);
    RUN(w3, u64, values, COUNT);
    RUN(w7, u64, values, COUNT);
    RUN(w10, u64, values, COUNT);
    RUN(wbillion, u64, values, COUNT);
    RUN(wodd, u64, values, COUNT);
    RUN(wmax, u64, values, COUNT);
    RUN(l3, i64, values, COUNT);
    RUN(l7, i64, values, COUNT);
    RUN(l10, i64, values, COUNT);
    RUN(lm7, i64, values, COUNT);
    RUN(lbillion, i64, values, COUNT);
    RUN(lbig, i64, values, COUNT);
    printf("sample %u %d %d %llu %lld\n", u7_q(4000000000u), s10_q(-2147483647 - 1), sm7_r(-50), w10_q(18446744073709551615ull),
           l7_q(-9223372036854775807ll));
    return 0;
}
