/*
 * Cross-Standard Test: C11 features compiled with -std=c89
 * Expected: Warnings/errors for C11 features used in C89 mode
 * 
 * Run with: ./mcc -std=c89 -fsyntax-only tests/cross/c11_in_c89.c
 * Expected output: Multiple warnings/errors about C11 extensions
 */

/* C11: _Alignas - should error */
_Alignas(16) int aligned_var;

/* C11: _Noreturn - should warn/error */
_Noreturn void abort_func(void);

/* C11: _Static_assert - should error */
_Static_assert(sizeof(int) >= 4, "int too small");

/* C11: _Generic - should error */
#define get_type(x) _Generic((x), int: 1, double: 2, default: 0)

int main(void)
{
    int x = 10;
    int type = get_type(x);
    (void)type;
    return 0;
}
