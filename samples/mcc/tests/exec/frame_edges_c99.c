/* Frame and addressing edges: a frame far beyond 32 KB, 64-bit accesses at
 * displacements that are not multiples of 4, remainders with many values
 * live, and calls through function pointers. */
#include <stdio.h>

struct record { char tag; short half; long long wide; int word; double real; };

/* 64-bit loads and stores two bytes past an 8-byte boundary. */
static long long odd_offsets(void)
{
    static long long storage[4];
    unsigned char *bytes = (unsigned char *)storage;
    long long *odd = (long long *)(bytes + 2);
    odd[0] = 0x1122334455667788LL;
    odd[1] = -42;
    return odd[0] + odd[1];
}

static volatile int seed = 3;

static int pick(int (*fn)(int, int), int a, int b) { return fn(a, b); }
static int rem7(int a, int b) { return (a % b) + (a % 7); }
static int add(int a, int b) { return a + b; }

static long long big_frame(int n)
{
    char area[40000];
    long long acc = 0;
    for (int i = 0; i < (int)sizeof(area); i++)
        area[i] = (char)(i * seed);
    for (int i = n; i < (int)sizeof(area); i += 997)
        acc += area[i];
    return acc + area[39999];
}

static int pressure(int x)
{
    int a = x + 1, b = x + 2, c = x + 3, d = x + 4, e = x + 5, f = x + 6, g = x + 7, h = x + 8;
    int i = x * 3, j = x * 5, k = x * 7, l = x * 9, m = x * 11, n = x * 13, o = x * 17, p = x * 19;
    int r = (a % b) + (c % d) * (e % 7) + (f % g) - (h % 5) + (i % j) + (k % 13) - (l % m) + (n % o) + (p % 23);
    unsigned u = (unsigned)(a * b) % (unsigned)(c + 1) + (unsigned)(i * j) % 1000u;
    return r + (int)u + a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p;
}

int main(void)
{
    struct record s[3];
    for (int i = 0; i < 3; i++) {
        s[i].tag = (char)('a' + i);
        s[i].half = (short)(1000 * i - 7);
        s[i].wide = 0x123456789LL * (i + seed);
        s[i].word = -i * 31;
        s[i].real = 1.5 * i;
    }
    long long wsum = 0;
    for (int i = 0; i < 3; i++)
        wsum += s[i].wide + s[i].half + s[i].word + (long long)s[i].real + s[i].tag;
    printf("record %lld %lld\n", wsum, odd_offsets());
    printf("frame %lld\n", big_frame(seed));
    printf("pressure %d %d\n", pressure(seed), pressure(-seed * 11));
    printf("calls %d %d\n", pick(rem7, 100, seed), pick(add, 40, 2));
    return 0;
}
