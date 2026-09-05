#include "variadic_abi.h"
#include <stdarg.h>

static int consume_int(va_list *cursor)
{
    return va_arg(*cursor, int);
}

double variadic_mixed(int count, ...)
{
    va_list cursor;
    va_list copy;
    va_start(cursor, count);
    va_copy(copy, cursor);
    int first = consume_int(&cursor);
    double second = va_arg(cursor, double);
    struct Small small = va_arg(cursor, struct Small);
    struct Large large = va_arg(cursor, struct Large);
    int *pointer = va_arg(cursor, int *);
    struct Odd odd = va_arg(cursor, struct Odd);
    if (va_arg(copy, int) != first)
        return -1000.0;

    va_end(copy);
    va_end(cursor);
    return count + first + second + small.value + large.a + large.b + large.c + *pointer + odd.bytes[2];
}

long long variadic_stack(int a, int b, int c, int d, int e, ...)
{
    va_list cursor;
    va_start(cursor, e);
    long long first = va_arg(cursor, long long);
    long long second = va_arg(cursor, long long);
    va_end(cursor);
    return a + b + c + d + e + first + second;
}

struct Large variadic_result(int tag, ...)
{
    va_list cursor;
    va_start(cursor, tag);
    double first = va_arg(cursor, double);
    struct Large value = va_arg(cursor, struct Large);
    value.a += tag;
    value.b += (long long)first;
    value.c += va_arg(cursor, int);
    va_end(cursor);
    return value;
}

double variadic_float(double first, ...)
{
    va_list cursor;
    va_start(cursor, first);
    double second = va_arg(cursor, double);
    double third = va_arg(cursor, double);
    double fourth = va_arg(cursor, double);
    double fifth = va_arg(cursor, double);
    va_end(cursor);
    return first + second + third + fourth + fifth;
}

int variadic_vla(int count, ...)
{
    volatile int values[count];
    va_list cursor;
    va_start(cursor, count);
    int i;
    int sum = 0;
    for (i = 0; i < count; i++)
    {
        values[i] = va_arg(cursor, int);
        sum += values[i];
    }

    va_end(cursor);
    return sum;
}
