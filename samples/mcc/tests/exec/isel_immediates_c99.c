/* Constants at the edges of instruction immediate fields (ADD/SUB/CMP
 * 12/16-bit ranges, logical bitmask patterns, shift counts, power-of-two
 * multipliers) in value and branch positions, for 32- and 64-bit operands. */
#include <stdio.h>

typedef long long i64;
typedef unsigned long long u64;

static volatile int sink_i;
static volatile i64 sink_l;

static int values32[] = { 0, 1, -1, 7, -7, 255, 256, 4095, 4096, -4095, -4096, 32767, 32768, -32768, 65535, 65536,
                          0x7fffffff, (int)0x80000000u, 0x12345678, -0x12345678 };
static i64 values64[] = { 0, 1, -1, 4095, 4096, -4096, 0xffffffffLL, 0x100000000LL, -0x100000000LL, 0x7fffffffffffffffLL,
                          0x123456789abcdefLL, -0x123456789abcdefLL };

#define COUNT(a) (int)(sizeof(a) / sizeof(a[0]))

static unsigned mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u + (h >> 7); }
static u64 mix64(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ULL + (h >> 11); }

static unsigned arith32(int x)
{
    unsigned h = 2166136261u;
    h = mix(h, (unsigned)(x + 1)); h = mix(h, (unsigned)(x - 1)); h = mix(h, (unsigned)(x + 4095)); h = mix(h, (unsigned)(x - 4095));
    h = mix(h, (unsigned)(x + 4096)); h = mix(h, (unsigned)(x - 4096)); h = mix(h, (unsigned)(x + 32767)); h = mix(h, (unsigned)(x - 32768));
    h = mix(h, (unsigned)(x + 65536)); h = mix(h, (unsigned)(x + 0x12345)); h = mix(h, (unsigned)(x + -1));
    h = mix(h, (unsigned)(x * 2)); h = mix(h, (unsigned)(x * 8)); h = mix(h, (unsigned)(x * 1024)); h = mix(h, (unsigned)(x * 3));
    h = mix(h, (unsigned)(x * -4)); h = mix(h, (unsigned)(x * 65536)); h = mix(h, (unsigned)(x * 1));
    h = mix(h, (unsigned)(x & 0xff)); h = mix(h, (unsigned)(x & 0xff00)); h = mix(h, (unsigned)(x & 0x0f0f0f0f)); h = mix(h, (unsigned)(x & 0x12345));
    h = mix(h, (unsigned)(x & -16)); h = mix(h, (unsigned)(x | 0x80000000u)); h = mix(h, (unsigned)(x | 0xaaaaaaaau)); h = mix(h, (unsigned)(x | 0x10001));
    h = mix(h, (unsigned)(x ^ 0xffff)); h = mix(h, (unsigned)(x ^ 0x55555555)); h = mix(h, (unsigned)(x ^ 0x1234)); h = mix(h, (unsigned)(x ^ -1));
    h = mix(h, (unsigned)x << 1); h = mix(h, (unsigned)x << 31); h = mix(h, (unsigned)x >> 5); h = mix(h, (unsigned)(x >> 5)); h = mix(h, (unsigned)(x >> 31));
    h = mix(h, (unsigned)(4095 - x)); h = mix(h, (unsigned)(1 + x)); h = mix(h, (unsigned)(0x0ff0 & x));
    return h;
}

static u64 arith64(i64 x)
{
    u64 h = 1469598103934665603ULL;
    h = mix64(h, (u64)(x + 1)); h = mix64(h, (u64)(x - 4095)); h = mix64(h, (u64)(x + 4096)); h = mix64(h, (u64)(x + 0x7fffffff));
    h = mix64(h, (u64)(x + 0x100000000LL)); h = mix64(h, (u64)(x - 0x80000000LL));
    h = mix64(h, (u64)x * 8); h = mix64(h, (u64)x * 0x100000000ULL); h = mix64(h, (u64)x * 4096); h = mix64(h, (u64)x * 5);
    h = mix64(h, (u64)(x & 0xffffffffLL)); h = mix64(h, (u64)(x & 0xff00ff00ff00ff00ULL)); h = mix64(h, (u64)(x & 0x7fffffffffffffffLL));
    h = mix64(h, (u64)(x & 0x123456789LL)); h = mix64(h, (u64)(x | 0x8000000000000000ULL)); h = mix64(h, (u64)(x ^ 0x5555555555555555LL));
    h = mix64(h, (u64)x << 33); h = mix64(h, (u64)x >> 40); h = mix64(h, (u64)(x >> 63)); h = mix64(h, (u64)(x >> 1));
    return h;
}

static unsigned compares32(int x)
{
    unsigned bits = 0, u = (unsigned)x;
    bits = bits << 1 | (x < 0);       bits = bits << 1 | (x <= 4095);  bits = bits << 1 | (x > -4096);
    bits = bits << 1 | (x >= 4096);   bits = bits << 1 | (x == 7);     bits = bits << 1 | (x != -1);
    bits = bits << 1 | (x < 32768);   bits = bits << 1 | (x > 65535);  bits = bits << 1 | (u < 4096u);
    bits = bits << 1 | (u >= 0x80000000u); bits = bits << 1 | (u > 255u); bits = bits << 1 | (u <= 65536u);
    bits = bits << 1 | (x == 0x12345678); bits = bits << 1 | (x < -32768); bits = bits << 1 | (u != 0u);
    return bits;
}

static unsigned branches32(int x)
{
    unsigned bits = 0, u = (unsigned)x;
    if (x < 0) bits |= 1u;
    if (x <= 4095) bits |= 2u;
    if (x > -4096) bits |= 4u;
    if (x >= 4096) bits |= 8u;
    if (x == 7) bits |= 16u;
    if (x != -1) bits |= 32u;
    if (u < 4096u) bits |= 64u;
    if (u >= 0x80000000u) bits |= 128u;
    if (u > 255u) bits |= 256u;
    if (x < 32768) bits |= 512u;
    if (x > 65535) bits |= 1024u;
    if (x == 0x12345678) bits |= 2048u;
    if (!(x < 100)) bits |= 4096u;
    return bits;
}

static unsigned branches64(i64 x)
{
    unsigned bits = 0;
    u64 u = (u64)x;
    if (x < 0) bits |= 1u;
    if (x <= 4095) bits |= 2u;
    if (x > -4096) bits |= 4u;
    if (x >= 0x100000000LL) bits |= 8u;
    if (x == -1) bits |= 16u;
    if (u < 4096u) bits |= 32u;
    if (u > 0xffffffffULL) bits |= 64u;
    if (x != 0x7fffffffffffffffLL) bits |= 128u;
    return bits;
}

static int loop_sum(int n)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        if ((i & 3) == 1 || i % 7 == 6)
            total += i * 4 - 3;
    for (int i = n; i > -40; i -= 9)
        total ^= i << 2;
    return total;
}

int main(void)
{
    unsigned h = 0;
    for (int i = 0; i < COUNT(values32); i++) {
        int x = values32[i];
        sink_i = x;
        h = mix(h, arith32(sink_i));
        h = mix(h, compares32(sink_i));
        h = mix(h, branches32(sink_i));
    }
    printf("i32 %08x\n", h);
    u64 g = 0;
    unsigned b = 0;
    for (int i = 0; i < COUNT(values64); i++) {
        sink_l = values64[i];
        g = mix64(g, arith64(sink_l));
        b = mix(b, branches64(sink_l));
    }
    printf("i64 %016llx %08x\n", g, b);
    printf("loop %d %d %d\n", loop_sum(0), loop_sum(17), loop_sum(1000));
    return 0;
}
