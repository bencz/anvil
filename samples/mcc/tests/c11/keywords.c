/*
 * C11 Test: New Keywords
 * Tests C11 new keywords and features
 */

/* C11: _Alignas specifier */
_Alignas(16) int aligned_int;
_Alignas(double) char aligned_char;

/* C11: _Alignof operator */
int align_of_int = _Alignof(int);
int align_of_double = _Alignof(double);

/* C11: _Noreturn function specifier */
_Noreturn void abort_func(void)
{
    while(1) { }  /* Never returns */
}

/* C11: _Static_assert */
_Static_assert(sizeof(int) >= 4, "int must be at least 4 bytes");
_Static_assert(sizeof(long) >= sizeof(int), "long must be >= int");

/* C11: _Thread_local storage class */
_Thread_local int thread_var = 0;
static _Thread_local int static_thread_var = 100;

/* C11: _Atomic type qualifier */
_Atomic int atomic_int;
_Atomic unsigned long atomic_ulong;

int main(void)
{
    /* _Alignof in expression */
    int a = _Alignof(int);
    int b = _Alignof(double);
    int c = _Alignof(aligned_int);
    
    /* _Static_assert in block scope */
    _Static_assert(sizeof(char) == 1, "char must be 1 byte");
    
    /* Use thread-local variable */
    thread_var = 42;
    static_thread_var = 200;
    
    /* Use atomic variable */
    atomic_int = 100;
    
    return a + b + c;
}
