#include <stdio.h>

typedef unsigned int u32;
typedef unsigned char u8;

static void vmax(int *c, const int *a, const int *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] > b[i] ? a[i] : b[i];
}

static void umin(u32 *c, const u32 *a, const u32 *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] < b[i] ? a[i] : b[i];
}

static void ucmp(u32 *c, const u32 *a, const u32 *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] >= b[i] ? 7u : (a[i] <= 3u ? a[i] : b[i]);
}

static void signs(int *c, const int *a, int t, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] <= t ? -1 : (a[i] != 5 ? a[i] : 99);
}

static void clampf(float *c, const float *a, float lo, float hi, int n)
{
    for (int i = 0; i < n; i++) {
        float v = a[i];
        if (v < lo)
            v = lo;
        if (v > hi)
            v = hi;
        c[i] = v;
    }
}

static void fpreds(float *c, const float *a, const float *b, int n)
{
    for (int i = 0; i < n; i++) {
        float x = a[i], y = b[i];
        float r = x == y ? 1.0f : 2.0f;
        r = x != y ? r + 4.0f : r;
        r = x >= y ? r + 8.0f : r;
        r = x <= y ? r + 16.0f : r;
        c[i] = r;
    }
}

static void dsel(double *c, const double *a, const double *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] < b[i] ? b[i] - a[i] : a[i] * 0.5;
}

static int ia[64], ib[64], ic[64];
static u32 ua[64], ub[64], uc[64];
static float fa[64], fb[64], fc[64];
static double da[64], db[64], dc[64];

static unsigned long mix(unsigned long h, long v)
{
    return (h ^ (unsigned long)v) * 1099511628211ul;
}

int main(void)
{
    float zero = 0.0f;
    float nan = zero / zero;
    for (int n = 0; n <= 40; n += 5) {
        for (int i = 0; i < 64; i++) {
            ia[i] = (i * 37) % 23 - 11;
            ib[i] = (i * 11) % 19 - 9;
            ua[i] = (u32)(i * 2654435761u);
            ub[i] = (u32)(i * 40503u) ^ 0x80000000u;
            fa[i] = (i % 7 == 3) ? nan : (float)((i * 13) % 17) - 8.0f;
            fb[i] = (i % 11 == 5) ? nan : (float)((i * 7) % 15) - 7.0f;
            da[i] = (double)((i * 5) % 9) - 4.0;
            db[i] = (double)((i * 3) % 7) - 3.0;
        }
        vmax(ic, ia, ib, n);
        umin(uc, ua, ub, n);
        ucmp(ub, ua, uc, n);
        signs(ib, ia, 2, n);
        clampf(fc, fa, -3.5f, 4.25f, n);
        fpreds(fb, fa, fc, n);
        dsel(dc, da, db, n);
        unsigned long h = 1469598103u;
        for (int i = 0; i < 64; i++) {
            h = mix(h, ic[i]);
            h = mix(h, (long)uc[i]);
            h = mix(h, (long)ub[i]);
            h = mix(h, ib[i]);
            h = mix(h, fc[i] != fc[i] ? 12345 : (long)(fc[i] * 4));
            h = mix(h, fb[i] != fb[i] ? 54321 : (long)(fb[i] * 4));
            h = mix(h, (long)(dc[i] * 8));
        }
        printf("n=%d %lu\n", n, h);
    }
    return 0;
}
