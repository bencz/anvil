#include <math.h>
#include <stdio.h>

/* sqrt and sqrtf run as the target's square-root instruction where it has
 * one; a negative operand still reaches the library (its domain error). */
static double norm(double x, double y, double z)
{
    return sqrt(x * x + y * y + z * z);
}

static void roots(double *out, const double *in, int n)
{
    for (int i = 0; i < n; i++)
        out[i] = sqrt(in[i]);
}

static float rootf(float x)
{
    return sqrtf(x);
}

static int is_nan(double x)
{
    return x != x;
}

int main(void)
{
    double in[41], out[41];
    double total = 0.0;
    for (int i = 0; i < 41; i++)
        in[i] = i * 0.75 + (i & 1 ? 1e-3 : 2.0);
    roots(out, in, 41);
    for (int i = 0; i < 41; i++)
        total += out[i];
    printf("norm %.15g\n", norm(1.0, 2.0, 2.0));
    printf("total %.15g\n", total);
    printf("rootf %.9g %.9g\n", rootf(2.0f), rootf(1e-30f));
    printf("zero %g %g\n", sqrt(0.0), sqrt(-0.0));
    printf("large %.15g tiny %.15g\n", sqrt(1e300), sqrt(4.9406564584124654e-324));
    double minus = -in[3];
    printf("negative nan %d %d\n", is_nan(sqrt(minus)), is_nan(sqrtf((float)minus)));
    printf("infinity %g\n", sqrt(1e308 * 10.0));
    return 0;
}
