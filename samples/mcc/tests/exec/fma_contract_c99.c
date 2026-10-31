#include <stdio.h>

static void saxpy(float *y, const float *x, float a, int n)
{
    for (int i = 0; i < n; i++)
        y[i] = a * x[i] + y[i];
}

static double dot(const double *a, const double *b, int n)
{
    double s = 0.25;
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

static double poly(double x)
{
    return ((2.0 * x + 3.0) * x - 1.5) * x + 0.5;
}

static double energy(const double *p, const double *v, double m, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) {
        double dx = p[3 * i], dy = p[3 * i + 1], dz = p[3 * i + 2];
        e += 0.5 * m * (v[i] * v[i]) + (dx * dx + dy * dy + dz * dz);
    }
    return e;
}

static float y[200], x[200];
static double a[200], b[200], p[600];

int main(void)
{
    unsigned long h = 0;
    for (int i = 0; i < 200; i++) {
        x[i] = (float)(i % 17) * 0.5f;
        a[i] = (double)(i % 23) - 11.0;
        b[i] = (double)(i % 7) * 0.25;
    }
    for (int i = 0; i < 600; i++)
        p[i] = (double)(i % 11) * 0.5 - 2.0;
    for (int n = 0; n < 200; n += 13) {
        for (int i = 0; i < 200; i++)
            y[i] = (float)(i % 5);
        saxpy(y, x, 1.5f, n);
        for (int i = 0; i < 200; i++)
            h = h * 31 + (unsigned long)(y[i] * 8.0f);
        h = h * 31 + (unsigned long)(long)(dot(a, b, n) * 64.0);
        h = h * 31 + (unsigned long)(long)(poly((double)n * 0.5) * 16.0);
        h = h * 31 + (unsigned long)(long)(energy(p, b, 2.0, n / 3) * 64.0);
    }
    printf("%lu\n", h);
    return 0;
}
