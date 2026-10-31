#include <stdio.h>

static void saxpy(float *y, const float *x, float a, int n)
{
    for (int i = 0; i < n; i++)
        y[i] = a * x[i] + y[i];
}

static void addv(double *c, const double *a, const double *b, int n)
{
    for (int i = 0; i < n; i++)
        c[i] = a[i] + b[i];
}

static void scale_from(float *v, int start, int end, float k)
{
    for (int i = start; i != end; i++)
        v[i] = v[i] * k - 1.5f;
}

static void divu(double *d, const double *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        d[i] = s[i] / 4.0;
}

static float f[64], g[64];
static double p[64], q[64], r[64];

static long checksum_f(const float *v, int n)
{
    long sum = 0;
    for (int i = 0; i < n; i++)
        sum = sum * 31 + (long)(v[i] * 16.0f);
    return sum;
}

static long checksum_d(const double *v, int n)
{
    long sum = 0;
    for (int i = 0; i < n; i++)
        sum = sum * 31 + (long)(v[i] * 16.0);
    return sum;
}

static void reset(void)
{
    for (int i = 0; i < 64; i++) {
        f[i] = (float)(i % 7) + 0.25f * (float)i;
        g[i] = (float)(i % 5) - 1.0f;
        p[i] = (double)i * 0.5 - 3.0;
        q[i] = (double)(i % 9) + 0.125;
        r[i] = 0;
    }
}

int main(void)
{
    for (int n = 0; n <= 19; n++) {
        reset();
        saxpy(f, g, 1.5f, n);
        addv(r, p, q, n);
        printf("n=%d %ld %ld\n", n, checksum_f(f, 64), checksum_d(r, 64));
    }
    /* Unaligned bases and overlapping streams. */
    for (int shift = -5; shift <= 5; shift++) {
        reset();
        saxpy(f + 8 + shift, f + 8, 2.0f, 23);
        addv(p + 8 + shift, p + 8, q, 21);
        addv(q + 3, q + 3, q + 3, 17);
        printf("shift=%d %ld %ld %ld\n", shift, checksum_f(f, 64), checksum_d(p, 64), checksum_d(q, 64));
    }
    reset();
    scale_from(f, 3, 30, 0.5f);
    scale_from(g, 10, 10, 2.0f);
    divu(r, q, 13);
    printf("misc %ld %ld %ld\n", checksum_f(f, 64), checksum_f(g, 64), checksum_d(r, 64));
    return 0;
}
