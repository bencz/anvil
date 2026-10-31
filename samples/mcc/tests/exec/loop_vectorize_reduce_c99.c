#include <stdio.h>

typedef unsigned int u32;
typedef unsigned char u8;

static int sum_i32(const int *a, int n)
{
    int s = 7;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}

static u32 xor_and(const u32 *a, int n, u32 *both)
{
    u32 x = 0x1234u, y = 0xffffffffu;
    for (int i = 0; i < n; i++) {
        x ^= a[i];
        y &= a[i] | 0x80000000u;
    }
    *both = y;
    return x;
}

static int max_i32(const int *a, int n)
{
    int m = -1000;
    for (int i = 0; i < n; i++)
        m = a[i] > m ? a[i] : m;
    return m;
}

static u32 min_u32(const u32 *a, int n)
{
    u32 m = 0xfffffff0u;
    for (int i = 0; i < n; i++)
        if (a[i] < m)
            m = a[i];
    return m;
}

static short sum_i16(const short *a, int n)
{
    short s = 0;
    for (int i = 0; i < n; i++)
        s = (short)(s + a[i]);
    return s;
}

static u8 max_u8(const u8 *a, int n)
{
    u8 m = 3;
    for (int i = 0; i < n; i++)
        m = a[i] > m ? a[i] : m;
    return m;
}

static float sum_f32(const float *a, int n)
{
    float s = 0.5f;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}

static int ia[300];
static u32 ua[300];
static short sa[300];
static u8 ba[300];
static float fa[300];

int main(void)
{
    unsigned long h = 0;
    for (int i = 0; i < 300; i++) {
        ia[i] = (i * 7919) % 2001 - 1000;
        ua[i] = (u32)i * 2654435761u;
        sa[i] = (short)((i * 31) % 4000 - 2000);
        ba[i] = (u8)(i * 37 + 11);
        fa[i] = (float)((i * 13) % 64) * 0.25f;
    }
    for (int n = 0; n <= 300; n += 23) {
        u32 both;
        h = h * 31 + (unsigned)sum_i32(ia, n);
        h = h * 31 + xor_and(ua, n, &both);
        h = h * 31 + both;
        h = h * 31 + (unsigned)max_i32(ia, n);
        h = h * 31 + min_u32(ua, n);
        h = h * 31 + (unsigned)(unsigned short)sum_i16(sa, n);
        h = h * 31 + max_u8(ba, n);
        h = h * 31 + (unsigned)(sum_f32(fa, n) * 4.0f);
    }
    printf("%lu\n", h);
    return 0;
}
