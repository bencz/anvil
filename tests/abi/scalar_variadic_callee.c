#include "scalar_variadic.h"
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

unsigned abi_cursor_size(void)
{
    return (unsigned)sizeof(va_list);
}

unsigned abi_cursor_offset(void)
{
    return (unsigned)offsetof(abi_cursor_record, cursor);
}

unsigned abi_cursor_record_size(void)
{
    return (unsigned)sizeof(abi_cursor_record);
}

int abi_cursor_next(va_list *cursor)
{
    return va_arg(*cursor, int);
}

double abi_variadic_sum(int count, double seed, ...)
{
    va_list arguments;
    va_list copy;
    va_start(arguments, seed);
    int first = va_arg(arguments, int);
    double fraction = va_arg(arguments, double);
    va_copy(copy, arguments);
    double sum = seed + first + fraction;
    for (int index = 1; index < count; index++)
    {
        int a = va_arg(arguments, int);
        double b = va_arg(arguments, double);
        int copied_a = va_arg(copy, int);
        double copied_b = va_arg(copy, double);
        if (a != copied_a || b != copied_b)
            return -9999;

        sum += a + b;
    }

    va_end(copy);
    va_end(arguments);
    return sum;
}

double abi_variadic_exhausted(int a, int b, int c, int d, int e, int f, int g,
                             double p, double q, double r, double s, double t, double u, double v, double w, double x, ...)
{
    va_list arguments;
    va_start(arguments, x);
    int first = va_arg(arguments, int);
    double second = va_arg(arguments, double);
    int third = va_arg(arguments, int);
    va_end(arguments);
    return a + b + c + d + e + f + g + p + q + r + s + t + u + v + w + x + first + second + third;
}

int abi_variadic_forward(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}
