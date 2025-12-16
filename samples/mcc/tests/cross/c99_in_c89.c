/*
 * Cross-Standard Test: C99 features compiled with -std=c89
 * Expected: Warnings for C99 features used in C89 mode
 * 
 * Run with: ./mcc -std=c89 -fsyntax-only tests/cross/c99_in_c89.c
 * Expected output: Multiple warnings about C99 extensions
 */

/* C99: inline keyword - should warn */
inline int add(int a, int b) { return a + b; }

/* C99: _Bool type - should warn */
_Bool flag = 1;

/* C99: restrict keyword - should warn */
void copy(int * restrict dst, const int * restrict src, int n)
{
    for (int i = 0; i < n; i++) {  /* C99: declaration in for - should warn */
        dst[i] = src[i];
    }
}

/* C99: long long - should warn */
long long big_number = 123456789012345LL;

/* C99: // comments - should warn (if not already stripped by preprocessor) */

/* C99: designated initializers */
struct point { int x, y; };
struct point p = { .x = 10, .y = 20 };

/* C99: compound literals */
int *get_array(void)
{
    return (int[]){1, 2, 3, 4, 5};
}

/* C99: variable length arrays (VLA) */
void vla_func(int n)
{
    int arr[n];  /* VLA - should warn */
    arr[0] = 0;
}

int main(void)
{
    return 0;
}
