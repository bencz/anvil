/* Dense double matrix multiply: FP multiply-add, 2D indexing, loop nests. */
#include <stdio.h>

#define N 256
static double a[N][N], b[N][N], c[N][N];

int main(void)
{
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = (double)((i * 7 + j * 3) % 17) / 16.0;
            b[i][j] = (double)((i * 5 + j * 11) % 13) / 12.0;
        }
    for (int rep = 0; rep < 4; rep++)
        for (int i = 0; i < N; i++)
            for (int k = 0; k < N; k++) {
                double aik = a[i][k];
                for (int j = 0; j < N; j++)
                    c[i][j] += aik * b[k][j];
            }
    double sum = 0.0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            sum += c[i][j] * (double)((i + j) & 7);
    printf("matmul %.6f\n", sum);
    return 0;
}
