/* Test: N-dimensional arrays and NxN matrix algebra.
 *
 * Fixed-size and variable-length arrays of 2, 3 and 4 dimensions:
 * nested and designated initializers, static storage, pointers to rows,
 * VLA parameters and the usual matrix operations (sum, difference,
 * product, transpose, trace, power, determinant), checked by printing
 * every result for comparison with the reference compiler. */

#include <stdio.h>

#define N 4

typedef struct {
    int rows, cols;
    long cell[N][N];
} matrix_t;

static const long g_identity[N][N] = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }
};
static long g_magic[N][N] = {
    { 16, 3, 2, 13 }, { 5, 10, 11, 8 }, { 9, 6, 7, 12 }, { 4, 15, 14, 1 }
};
/* Brace elision and designators across three dimensions. */
static int g_cube[2][3][4] = {
    { { 1, 2, 3, 4 }, { 5, 6 }, [2][3] = 9 },
    [1] = { [1] = { 7, 7, 7, 7 }, 1, 2 }
};
static int g_tensor[2][2][3][2] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24
};

static void print_matrix(const char *name, int n, long m[n][n])
{
    printf("%s:", name);
    for (int i = 0; i < n; i++) {
        printf(" [");
        for (int j = 0; j < n; j++)
            printf(j ? " %ld" : "%ld", m[i][j]);
        printf("]");
    }
    printf("\n");
}

static void mat_add(int n, long a[n][n], long b[n][n], long r[n][n])
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            r[i][j] = a[i][j] + b[i][j];
}

static void mat_sub(int n, long a[n][n], long b[n][n], long r[n][n])
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            r[i][j] = a[i][j] - b[i][j];
}

static void mat_mul(int n, long a[n][n], long b[n][n], long r[n][n])
{
    long tmp[n][n];                     /* r may alias a or b */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            long sum = 0;
            for (int k = 0; k < n; k++)
                sum += a[i][k] * b[k][j];
            tmp[i][j] = sum;
        }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            r[i][j] = tmp[i][j];
}

static void mat_transpose(int n, long a[n][n], long r[n][n])
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            r[j][i] = a[i][j];
}

static long mat_trace(int n, long a[n][n])
{
    long t = 0;
    for (int i = 0; i < n; i++)
        t += a[i][i];
    return t;
}

static void mat_pow(int n, long a[n][n], int e, long r[n][n])
{
    long base[n][n];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            base[i][j] = a[i][j];
            r[i][j] = i == j;
        }
    while (e > 0) {
        if (e & 1)
            mat_mul(n, r, base, r);
        mat_mul(n, base, base, base);
        e >>= 1;
    }
}

/* Exact integer determinant (Bareiss fraction-free elimination). */
static long mat_det(int n, long a[n][n])
{
    long m[n][n];
    long sign = 1, prev = 1;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            m[i][j] = a[i][j];
    for (int k = 0; k < n - 1; k++) {
        if (m[k][k] == 0) {
            int swap = -1;
            for (int i = k + 1; i < n && swap < 0; i++)
                if (m[i][k] != 0) swap = i;
            if (swap < 0) return 0;
            for (int j = 0; j < n; j++) {
                long t = m[k][j];
                m[k][j] = m[swap][j];
                m[swap][j] = t;
            }
            sign = -sign;
        }
        for (int i = k + 1; i < n; i++)
            for (int j = k + 1; j < n; j++)
                m[i][j] = (m[i][j] * m[k][k] - m[i][k] * m[k][j]) / prev;
        prev = m[k][k];
    }
    return sign * m[n - 1][n - 1];
}

/* A 3D batch: one matrix product per slice. */
static void batch_mul(int b, int n, long x[b][n][n], long y[b][n][n],
                      long r[b][n][n])
{
    for (int s = 0; s < b; s++)
        mat_mul(n, x[s], y[s], r[s]);
}

/* 4D tensor contraction: out[i][l] = sum over j,k of t[i][j][k][l]. */
static void contract(int a, int b, int c, int d, int t[a][b][c][d],
                     long out[a][d])
{
    for (int i = 0; i < a; i++)
        for (int l = 0; l < d; l++) {
            long sum = 0;
            for (int j = 0; j < b; j++)
                for (int k = 0; k < c; k++)
                    sum += t[i][j][k][l];
            out[i][l] = sum;
        }
}

static long row_sum(const long *row, int n)
{
    long s = 0;
    for (int i = 0; i < n; i++) s += row[i];
    return s;
}

static int call_count(void)
{
    static int history[3][2] = { { 1, 2 } };
    history[2][1] += 5;
    return history[0][0] + history[0][1] + history[2][1];
}

int main(void)
{
    long a[N][N], b[N][N], r[N][N], t[N][N];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = i * N + j + 1;
            b[i][j] = (i == j) ? 2 : (i + j) % 3 - 1;
        }

    print_matrix("a", N, a);
    print_matrix("b", N, b);
    mat_add(N, a, b, r);  print_matrix("a+b", N, r);
    mat_sub(N, a, b, r);  print_matrix("a-b", N, r);
    mat_mul(N, a, b, r);  print_matrix("a*b", N, r);
    mat_mul(N, b, a, r);  print_matrix("b*a", N, r);
    mat_transpose(N, a, t); print_matrix("a^T", N, t);
    mat_mul(N, a, (long (*)[N])g_identity, r); print_matrix("a*I", N, r);
    printf("trace a=%ld b=%ld magic=%ld\n", mat_trace(N, a), mat_trace(N, b),
           mat_trace(N, g_magic));
    printf("det a=%ld b=%ld magic=%ld I=%ld\n", mat_det(N, a), mat_det(N, b),
           mat_det(N, g_magic), mat_det(N, (long (*)[N])g_identity));
    mat_pow(N, b, 5, r);  print_matrix("b^5", N, r);

    /* Every size from 1 to 6 through the same VLA routines. */
    for (int n = 1; n <= 6; n++) {
        long x[n][n], y[n][n], z[n][n];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                x[i][j] = (i + 1) * (j + 2) % 7 - 3;
                y[i][j] = i == j ? n : j - i;
            }
        mat_mul(n, x, y, z);
        mat_pow(n, y, 3, x);
        printf("n=%d det(y)=%ld trace(xy)=%ld trace(y^3)=%ld z[n-1][0]=%ld\n",
               n, mat_det(n, y), mat_trace(n, z), mat_trace(n, x), z[n - 1][0]);
    }

    /* 3D: a batch of matrices, fixed and variable-length. */
    long batch_x[3][N][N], batch_y[3][N][N], batch_r[3][N][N];
    for (int s = 0; s < 3; s++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                batch_x[s][i][j] = s + i - j;
                batch_y[s][i][j] = (s + 1) * (i == j) + (j == i + 1);
            }
    batch_mul(3, N, batch_x, batch_y, batch_r);
    for (int s = 0; s < 3; s++)
        printf("batch[%d] trace=%ld corner=%ld\n", s,
               mat_trace(N, batch_r[s]), batch_r[s][N - 1][N - 1]);

    int planes = 2, rows = 3, cols = 4;
    int vcube[planes][rows][cols];
    for (int p = 0; p < planes; p++)
        for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++)
                vcube[p][i][j] = g_cube[p][i][j] * (p + 1);
    long cube_sum = 0;
    for (int p = 0; p < planes; p++)
        for (int i = 0; i < rows; i++)
            for (int j = 0; j < cols; j++)
                cube_sum += vcube[p][i][j] * (i + 1) * (j + 1);
    printf("cube g[0][1][1]=%d g[0][2][3]=%d g[1][1][2]=%d g[1][2][0]=%d sum=%ld\n",
           g_cube[0][1][1], g_cube[0][2][3], g_cube[1][1][2], g_cube[1][2][0],
           cube_sum);

    /* 4D: fixed tensor, a variable-length copy and a contraction. */
    int d0 = 2, d1 = 2, d2 = 3, d3 = 2;
    int vt[d0][d1][d2][d3];
    for (int i = 0; i < d0; i++)
        for (int j = 0; j < d1; j++)
            for (int k = 0; k < d2; k++)
                for (int l = 0; l < d3; l++)
                    vt[i][j][k][l] = g_tensor[i][j][k][l] - (i ^ k);
    long contracted[2][2];
    contract(d0, d1, d2, d3, vt, contracted);
    long (*fixed)[2] = (long (*)[2])r;
    contract(2, 2, 3, 2, g_tensor, fixed);
    printf("tensor %d %d contracted [%ld %ld][%ld %ld] fixed [%ld %ld][%ld %ld]\n",
           g_tensor[1][0][2][1], vt[1][1][1][0],
           contracted[0][0], contracted[0][1], contracted[1][0], contracted[1][1],
           fixed[0][0], fixed[0][1], fixed[1][0], fixed[1][1]);

    int local4[2][2][2][2] = { { { { 1 } }, { { 2, 3 }, [1] = { 4, 5 } } },
                               [1][1][1][1] = 9 };
    int total4 = 0;
    for (int i = 0; i < 16; i++)
        total4 += local4[i >> 3][(i >> 2) & 1][(i >> 1) & 1][i & 1] * (i + 1);
    printf("local4 total=%d last=%d\n", total4, local4[1][1][1][1]);

    /* Rows as pointers, pointers to rows, matrices inside structs. */
    long (*row_ptr)[N] = g_magic;
    printf("magic rows %ld %ld %ld %ld cols %ld\n",
           row_sum(g_magic[0], N), row_sum(row_ptr[1], N),
           row_sum(*(row_ptr + 2), N), row_sum(&g_magic[3][0], N),
           g_magic[0][3] + g_magic[1][3] + g_magic[2][3] + g_magic[3][3]);
    matrix_t m = { N, N, { [0] = { 1, 2 }, [3][3] = 7 } };
    matrix_t copy = m;
    copy.cell[1][1] = 5;
    mat_mul(N, m.cell, copy.cell, r);
    printf("struct %d %d %ld %ld %ld\n", m.rows, copy.cols, m.cell[3][3],
           r[0][1], r[3][3]);

    call_count();
    call_count();
    printf("static %d\n", call_count());
    return 0;
}
