/* Test: pointer arithmetic on pointers to variable-length arrays and on
 * decayed arrays: +, -, +=, -=, ++, --, and pointer differences. */
#include <stdio.h>
int main(void)
{
    int a[10], n = 3, rows = 4, i, j;
    for (i = 0; i < 10; i++) a[i] = i * i;
    int *p = a;
    p += 3; p -= 1;
    int *q = p++;
    ++q;
    printf("p %d q %d diff %d\n", *p, *q, (int)(p - a));
    int m[rows][n];
    for (i = 0; i < rows; i++) for (j = 0; j < n; j++) m[i][j] = i * 10 + j;
    int (*r)[n] = m;
    r = r + 2;
    printf("r %d %d\n", r[0][1], (*r)[2]);
    r -= 1; printf("r %d\n", r[0][0]);
    r++; ++r; printf("r %d\n", (*r)[1]);
    r--; printf("r %d diff %d\n", r[0][2], (int)(r - m));
    int (*s)[n] = 1 + m;
    printf("s %d\n", s[1][1]);
    return 0;
}
