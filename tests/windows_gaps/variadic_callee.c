#include <stdarg.h>
#include <stdio.h>

static int sum(int count, ...)
{
    va_list arguments;
    int result = 0;
    int index;
    va_start(arguments, count);

    for (index = 0; index < count; index++)
        result += va_arg(arguments, int);

    va_end(arguments);
    return result;
}

int main(void)
{
    int result = sum(2, 17, 25);
    printf("variadic callee: expected 42, received %d\n", result);
    return result == 42 ? 0 : 1;
}
