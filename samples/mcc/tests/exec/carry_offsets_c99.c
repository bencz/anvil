/* x + (a < b) and x + (a > b) on unsigned 32- and 64-bit values (a carry
 * added where the target has the form), and accesses q[i + k] whose
 * constant k joins the indexed access's displacement: the C values on
 * every target. */
#include <stdio.h>

static unsigned below(unsigned x, unsigned a, unsigned b) { return x + (a < b); }
static unsigned above(unsigned x, unsigned a, unsigned b) { return x + (a > b); }
static unsigned long long below64(unsigned long long x, unsigned long long a, unsigned long long b) { return x + (a < b); }
static unsigned long long above64(unsigned long long x, unsigned long long a, unsigned long long b) { return x + (a > b); }

static void neighbours(long long *q, long i)
{
    q[i] = 1;
    q[i + 1] = 2;
    q[i + 3] = q[i] + q[i + 1];
}

int main(void)
{
    static const unsigned long long values[] = { 0, 1, 2, 0x7fffffff, 0x80000000u, 0xffffffffu, 0x100000000ULL, 0xffffffffffffffffULL };
    unsigned n = sizeof(values) / sizeof(values[0]);
    unsigned long long sum = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < n; j++) {
            unsigned a = (unsigned)values[i], b = (unsigned)values[j];
            sum = sum * 3 + below(i, a, b) + 5 * above(j, a, b);
            sum = sum * 3 + below64(values[i], values[i], values[j]) + 7 * above64(values[j], values[i], values[j]);
        }
    printf("%llu %u %u\n", sum, below(0xffffffffu, 1, 2), above(0xffffffffu, 2, 1));
    long long q[8] = { 0 };
    for (long i = 0; i < 4; i++) {
        neighbours(q, i);
        printf("%lld %lld %lld %lld\n", q[i], q[i + 1], q[i + 2], q[i + 3]);
    }
    return 0;
}
