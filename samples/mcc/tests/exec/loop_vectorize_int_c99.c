#include <stdio.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

static void add_i32(int *c, const int *a, const int *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] + b[i] * 3 - 7;
}

static void mix_u32(u32 *c, const u32 *a, int k, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = (a[i] << 3) ^ (a[i] >> k) ^ 0x5a5a5a5au;
}

static void shift_i32(int *c, const int *a, int k, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = (a[i] >> k) | (a[i] & 0xff);
}

static void bytes(u8 *d, const u8 *s, u8 bias, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = (u8)(s[i] + bias) ^ d[i];
}

static void words(short *d, const short *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = (short)(s[i] * d[i] - s[i]);
}

static void quads(u64 *d, const u64 *s, int n)
{
    for (int i = 0; i < n; i++)
        d[i] = (s[i] << 5) + (d[i] >> 2) - s[i];
}

static void tiny(int *d)
{
    for (int i = 0; i < 3; i++)
        d[i] = d[i] * 5 + 1;
}

static int a[80], b[80], c[80];
static u32 ua[80], uc[80];
static u8 ba[96], bb[96];
static short sa[80], sb[80];
static u64 qa[48], qb[48];

static unsigned long hash(const void *p, int bytes_)
{
    const u8 *v = p;
    unsigned long h = 1469598103u;
    for (int i = 0; i < bytes_; i++)
        h = (h ^ v[i]) * 16777619u;
    return h;
}

static void reset(void)
{
    for (int i = 0; i < 80; i++) {
        a[i] = i * 37 - 900;
        b[i] = (i % 11) - 5;
        c[i] = 0;
        ua[i] = (u32)i * 2654435761u;
        uc[i] = 0;
        sa[i] = (short)(i * 113 - 4000);
        sb[i] = (short)(i % 7 + 1);
    }
    for (int i = 0; i < 96; i++) {
        ba[i] = (u8)(i * 7);
        bb[i] = (u8)(255 - i);
    }
    for (int i = 0; i < 48; i++) {
        qa[i] = (u64)i * 0x9E3779B97F4A7C15ull;
        qb[i] = ~(u64)i;
    }
}

int main(void)
{
    for (int n = 0; n <= 37; n += 3) {
        reset();
        add_i32(c, a, b, n);
        mix_u32(uc, ua, n % 9, n);
        shift_i32(b, a, (n % 5) + 1, n);
        bytes(bb, ba, (u8)(n * 3), n * 2);
        words(sb, sa, n);
        quads(qb, qa, n);
        printf("n=%d %lu %lu %lu %lu %lu %lu\n", n, hash(c, sizeof c), hash(uc, sizeof uc), hash(b, sizeof b), hash(bb, sizeof bb),
               hash(sb, sizeof sb), hash(qb, sizeof qb));
    }
    for (int shift = -6; shift <= 6; shift++) {
        reset();
        add_i32(a + 10 + shift, a + 10, b, 41);
        bytes(ba + 20 + shift, ba + 20, 9, 51);
        quads(qa + 8 + shift, qa + 8, 23);
        printf("shift=%d %lu %lu %lu\n", shift, hash(a, sizeof a), hash(ba, sizeof ba), hash(qa, sizeof qa));
    }
    reset();
    tiny(c);
    printf("tiny %d %d %d\n", c[0], c[1], c[2]);
    return 0;
}
