#include <stdio.h>

/* C clamps of widened sums and differences to the narrow type's range: at
 * O3 each becomes one saturating lane operation (paddusb, uqsub, vaddshs...)
 * and must match the scalar loop; a clamp to another bound stays exact. */
typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef short i16;

enum { N = 1000 };
static u8 a[N], b[N], c[N];
static i8 g[N], h[N], k[N];
static u16 p[N], q[N], r[N];
static i16 x[N], y[N], z[N];

static void add_u8(int n)
{
    for (int i = 0; i < n; i++) {
        int t = a[i] + b[i];
        c[i] = t > 255 ? 255 : t;
    }
}

static void sub_u8(int n)
{
    for (int i = 0; i < n; i++) {
        int t = a[i] - b[i];
        c[i] = t < 0 ? 0 : t;
    }
}

static void add_u8_almost(int n)
{
    for (int i = 0; i < n; i++) {
        int t = a[i] + b[i];
        c[i] = t > 254 ? 254 : t;
    }
}

static void add_u16(int n)
{
    for (int i = 0; i < n; i++) {
        int t = p[i] + q[i];
        if (t > 65535)
            t = 65535;
        r[i] = t;
    }
}

static void sub_u16(int n)
{
    for (int i = 0; i < n; i++) {
        int t = p[i] - q[i];
        r[i] = 0 > t ? 0 : t;
    }
}

static void add_i16(int n)
{
    for (int i = 0; i < n; i++) {
        int t = x[i] + y[i];
        if (t > 32767)
            t = 32767;
        if (t < -32768)
            t = -32768;
        z[i] = t;
    }
}

static void sub_i8(int n)
{
    for (int i = 0; i < n; i++) {
        int t = g[i] - h[i];
        t = t < -128 ? -128 : t;
        t = t >= 127 ? 127 : t;
        k[i] = t;
    }
}

static unsigned hash(const unsigned char *bytes, int count)
{
    unsigned value = 2166136261u;
    for (int i = 0; i < count; i++)
        value = (value ^ bytes[i]) * 16777619u;
    return value;
}

/* Wide arrays hashed by value, so the output is the same on every byte
 * order. */
static unsigned hash16(const u16 *values, int count)
{
    unsigned value = 2166136261u;
    for (int i = 0; i < count; i++)
        value = (value ^ values[i]) * 16777619u;
    return value;
}

int main(void)
{
    unsigned s = 1;
    for (int i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        a[i] = (u8)(s >> 16);
        b[i] = (u8)(s >> 9);
        g[i] = (i8)(s >> 5);
        h[i] = (i8)(s >> 19);
        p[i] = (u16)(s >> 7);
        q[i] = (u16)(s >> 13);
        x[i] = (i16)(s >> 3);
        y[i] = (i16)(s >> 11);
    }
    /* The extremes of every type. */
    a[0] = 255; b[0] = 255; g[0] = -128; h[0] = 127; p[0] = 65535; q[0] = 65535; x[0] = 32767; y[0] = 32767;
    a[1] = 0; b[1] = 255; g[1] = 127; h[1] = -128; p[1] = 0; q[1] = 65535; x[1] = -32768; y[1] = -32768;
    for (int n = 2; n <= N; n += 333) {
        add_u8(n);
        printf("%u ", hash(c, n));
        sub_u8(n);
        printf("%u ", hash(c, n));
        add_u8_almost(n);
        printf("%u ", hash(c, n));
        add_u16(n);
        printf("%u ", hash16(r, n));
        sub_u16(n);
        printf("%u ", hash16(r, n));
        add_i16(n);
        printf("%u ", hash16((const u16 *)z, n));
        sub_i8(n);
        printf("%u\n", hash((const unsigned char *)k, n));
    }
    printf("%d %d %d %d %d\n", c[0], k[0], k[1], z[0], z[1]);
    return 0;
}
