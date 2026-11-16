/* C11 6.4.4.1p5: an integer constant has the first type of its list that
 * represents its value.  Decimal constants without u stay signed (int, long,
 * long long); octal and hexadecimal ones may be unsigned (0xffffffff is an
 * unsigned int where int has 32 bits, so widening it does not extend its
 * sign).  The output is the same on ILP32 and LP64 targets (only
 * 0xffffffffL changes type between them, printed as its rule). */
#include <stdio.h>

#define SIGNED(e) ((e) * 0 - 1 < 0)
#define SHOW(e) printf("%s size %u signed %d\n", #e, (unsigned)sizeof(e), SIGNED(e))

static unsigned long long low32(unsigned long long x) { return x & 0xffffffff; }
static unsigned long low32_long(unsigned long x) { return x & 0xffffffff; }
static long long widen_hex(void) { long long v = 0x80000000; return v; }
static long long widen_decimal(void) { long long v = 2147483648; return v; }
static int compare_hex(int x) { return x < 0xffffffff; }

int main(void)
{
    SHOW(0x7fffffff);
    SHOW(0x80000000);
    SHOW(0xffffffff);
    SHOW(2147483647);
    SHOW(2147483648);
    SHOW(4294967295);
    SHOW(4294967296);
    SHOW(0x100000000);
    SHOW(0xffffffffu);
    /* A long of 32 bits cannot hold it: unsigned long there. */
    printf("0xffffffffL signed %d\n", SIGNED(0xffffffffL) == (sizeof(long) > 4));
    SHOW(4294967295L);
    SHOW(0x7fffffffffffffff);
    SHOW(0x8000000000000000);
    SHOW(9223372036854775807);
    SHOW(0x8000000000000000LL);
    SHOW(017777777777);
    SHOW(037777777777);
    printf("%llx %lx\n", low32(0x123456789abcdefULL), low32_long((unsigned long)-1));
    printf("%llx %lld\n", (unsigned long long)widen_hex(), widen_decimal());
    printf("%d %d\n", compare_hex(-1), compare_hex(5));
    printf("%d %d\n", 0x80000000 > 0, -0x80000000 > 0);
    return 0;
}
