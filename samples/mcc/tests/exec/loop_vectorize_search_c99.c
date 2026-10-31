#include <stdio.h>

typedef unsigned char u8;

static int find_byte(const u8 *s, u8 c, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] == c)
            return i;
    return -1;
}

static int mismatch(const u8 *a, const u8 *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            break;
    return i;
}

/* Two early exits: a difference, or a terminator in the first stream. */
static int prefix(const u8 *a, const u8 *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            break;
        if (a[i] == 0)
            break;
    }
    return i;
}

static int first_above(const int *v, int limit, int n)
{
    for (int i = 0; i < n; i++)
        if (v[i] > limit)
            return i;
    return -1;
}

static int run_below(const short *v, int n)
{
    int i = 0;
    while (i < n && v[i] < 100)
        i++;
    return i;
}

static int first_nan_or_big(const double *v, int n)
{
    for (int i = 0; i < n; i++)
        if (!(v[i] <= 1000.0))
            return i;
    return -1;
}

/* Storage with room to start each array on a 16-byte boundary. */
static u8 store_a[316], store_b[316];
static int store_i[204];
static short store_s[208];
static double store_d[102];

static void *align16(void *p)
{
    unsigned long address = (unsigned long)p;
    return (void *)(address + (16 - address % 16) % 16);
}

int main(void)
{
    u8 *buf_a = align16(store_a), *buf_b = align16(store_b);
    int *ints = align16(store_i);
    short *shorts = align16(store_s);
    double *doubles = align16(store_d);
    unsigned long h = 0;
    for (int i = 0; i < 300; i++)
        buf_a[i] = buf_b[i] = (u8)(i * 7 + 1);
    for (int off = 0; off < 3; off++)
        for (int pos = 0; pos < 70; pos += 3) {
            buf_a[off + pos] ^= 0x55;
            h = h * 31 + (unsigned)mismatch(buf_a + off, buf_b + off, 67);
            h = h * 31 + (unsigned)find_byte(buf_a + off, buf_a[off + pos], 64);
            h = h * 31 + (unsigned)find_byte(buf_a + off, 0xEE, 61);
            buf_a[off + pos] ^= 0x55;
            buf_a[off + pos + 1] = 0;
            buf_b[off + pos + 1] = 0;
            h = h * 31 + (unsigned)prefix(buf_a + off, buf_b + off, 100);
            buf_a[off + pos + 1] = buf_b[off + pos + 1] = (u8)((off + pos + 1) * 7 + 1);
        }
    for (int i = 0; i < 200; i++) {
        ints[i] = (i * 37) % 101;
        shorts[i] = (short)((i * 13) % 97);
    }
    for (int n = 0; n < 150; n += 7) {
        h = h * 31 + (unsigned)first_above(ints, 95 - n % 9, n);
        shorts[n] = 150;
        h = h * 31 + (unsigned)run_below(shorts, 180);
        shorts[n] = (short)((n * 13) % 97);
    }
    for (int i = 0; i < 100; i++)
        doubles[i] = i * 3.5;
    doubles[77] = 0.0 / 0.0;
    for (int n = 0; n < 100; n += 9)
        h = h * 31 + (unsigned)first_nan_or_big(doubles, n);
    printf("%lu\n", h);
    return 0;
}
