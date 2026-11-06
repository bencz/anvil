/* Matrix product through pointers with a run-time order: flat indices
 * i * n + j in int, the form of dynamically sized matrices. */
#include <stdio.h>

#define MAX 256
static double a[MAX * MAX], b[MAX * MAX], c[MAX * MAX];
static volatile int order = MAX;

static void multiply(double *out, const double *left, const double *right, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            double lik = left[i * n + k];
            for (int j = 0; j < n; j++)
                out[i * n + j] += lik * right[k * n + j];
        }
}

int main(void)
{
    int n = order;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            a[i * n + j] = (double)((i * 7 + j * 3) % 17) / 16.0;
            b[i * n + j] = (double)((i * 5 + j * 11) % 13) / 12.0;
        }
    for (int rep = 0; rep < 4; rep++)
        multiply(c, a, b, n);
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            sum += c[i * n + j] * (double)((i + j) & 7);
    printf("matflat %.6f\n", sum);
    return 0;
}
