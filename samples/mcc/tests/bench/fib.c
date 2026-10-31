/* Recursive calls: prologue/epilogue and argument passing cost. */
#include <stdio.h>

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

int main(void)
{
    printf("fib %d\n", fib(35));
    return 0;
}
