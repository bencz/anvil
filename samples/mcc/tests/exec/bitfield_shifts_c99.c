/* Field extractions and insertions written with shifts and masks: a field
 * moved to bit 0, a low field moved up, signed fields through a left then
 * an arithmetic right shift (either amount larger), on 32 and 64 bits and
 * through a widening (left shifts on unsigned values, so no overflow).  One
 * bitfield or rotate-and-mask instruction where the target has it; the C
 * values on every target. */
#include <stdio.h>

static unsigned extract4(unsigned x) { return (x >> 4) & 15; }
static unsigned extract_top(unsigned x) { return (x >> 20) & 0xfff; }
static unsigned insert8(unsigned x) { return (x & 255) << 3; }
static unsigned long long insert_wide(unsigned x) { return (unsigned long long)(x & 0xffff) << 12; }
static unsigned long long extract_wide(unsigned long long x) { return (x >> 40) & 0xffff; }
static unsigned long long insert_high(unsigned long long x) { return (x & 0xff) << 48; }
static int signed_field(int x) { return (int)((unsigned)x << 20) >> 26; }
static int signed_insert(int x) { return (int)((unsigned)x << 26) >> 20; }
static int signed_low(int x) { return (int)((unsigned)x << 27) >> 27; }
static long long signed_wide(long long x) { return (long long)((unsigned long long)x << 40) >> 50; }
static long long signed_wide_insert(long long x) { return (long long)((unsigned long long)x << 60) >> 52; }
static unsigned shift_pair(unsigned x) { return (x << 8) >> 4; }
static unsigned shift_pair_back(unsigned x) { return (x >> 8) << 4; }

int main(void)
{
    static const unsigned long long inputs[] = { 0, 1, 0x7f, 0x80, 0xff, 0x1234, 0xffff, 0x8000, 0x12345678, 0x80000000u, 0xffffffffu,
                                                0xdeadbeefcafebabeULL, 0x8000000000000000ULL, 0xffffffffffffffffULL, 0x0123456789abcdefULL };
    unsigned long long sum = 0;
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        unsigned long long v = inputs[i];
        unsigned u = (unsigned)v;
        int s = (int)u;
        printf("%u %u %u %llu %llu %llu %d %d %d %lld %lld %u %u\n", extract4(u), extract_top(u), insert8(u), insert_wide(u), extract_wide(v),
               insert_high(v), signed_field(s), signed_insert(s), signed_low(s), signed_wide((long long)v), signed_wide_insert((long long)v),
               shift_pair(u), shift_pair_back(u));
        sum += extract4(u) + insert8(u) + (unsigned long long)signed_field(s) + (unsigned long long)signed_wide((long long)v);
    }
    printf("%llu\n", sum);
    return 0;
}
