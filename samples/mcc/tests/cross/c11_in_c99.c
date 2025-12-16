/*
 * Cross-Standard Test: C11 features compiled with -std=c99
 * Expected: Warnings/errors for C11 features used in C99 mode
 * 
 * Run with: ./mcc -std=c99 -fsyntax-only tests/cross/c11_in_c99.c
 * Expected output: Multiple warnings about C11 extensions
 */

/* C11: _Alignas - should warn/error */
_Alignas(16) int aligned_var;

/* C11: _Alignof - should warn */
int alignment = _Alignof(double);

/* C11: _Noreturn - should warn */
_Noreturn void abort_func(void);

/* C11: _Static_assert - should error */
_Static_assert(sizeof(int) >= 4, "int too small");

/* C11: _Atomic - should warn */
_Atomic int atomic_counter;

/* C11: _Thread_local - should warn/error */
_Thread_local int thread_var;

/* C11: _Generic - should error */
#define type_name(x) _Generic((x), \
    int: "int", \
    double: "double", \
    default: "other")

/* C11: Anonymous struct/union members */
struct outer {
    int a;
    struct {
        int x;
        int y;
    };  /* Anonymous - should warn */
};

int main(void)
{
    int x = 10;
    const char *name = type_name(x);
    (void)name;
    
    struct outer o;
    o.x = 1;  /* Access anonymous member */
    
    return 0;
}
