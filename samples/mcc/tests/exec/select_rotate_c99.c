#include <stdio.h>

/* Integer selects become conditional moves fused with their compare
 * (cmov, csel, isel, LOCGR), rotate idioms become rotate instructions, and
 * small leaves run without a frame: every result must match. */
typedef unsigned int u32;
typedef unsigned long long u64;

static int imax(int a, int b) { return a > b ? a : b; }
static unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
static long long clampll(long long x, long long lo, long long hi) { return x < lo ? lo : x > hi ? hi : x; }
static int iabs(int x) { return x < 0 ? -x : x; }
static int pick(double a, double b, int x, int y) { return a > b ? x : y; }
static int sign(int x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
static int eq_pick(int a, int b, int x) { return a == b ? x : x + 1; }

static u32 rotl32(u32 x, int k) { return (x << k) | (x >> (32 - k)); }
static u32 rotr7(u32 x) { return (x >> 7) | (x << 25); }
static u64 rotl64(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
static u64 mix64(u64 h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    return (h << 31) | (h >> 33);
}

int main(void)
{
    long long total = 0;
    for (int i = -60; i < 60; i++)
        total += imax(i, 7 - i) * 3 + umin((unsigned)i, 20u) + clampll(i * 3, -40, 40) + iabs(i) + pick(i * 0.5, 3.0, 11, 13) + sign(i) +
                 eq_pick(i, 5, i);
    u32 h = 0x12345678;
    u64 g = 0x123456789abcdefull;
    for (int i = 1; i < 1000; i++) {
        h = rotl32(h ^ (u32)i, i % 31 + 1) + rotr7(h);
        g = rotl64(g + (u64)i, i % 63 + 1) ^ mix64(g);
    }
    printf("%lld %d %d %u %llu\n", total, imax(-1, -2), sign(-9), h, g);
    return 0;
}
