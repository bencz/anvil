#include <stdarg.h>
#include <stdio.h>

static double consume(int count, va_list arguments)
{
    double sum = 0;
    for (int index = 0; index < count; index++)
    {
        int integer = va_arg(arguments, int);
        double fraction = va_arg(arguments, double);
        sum += integer + fraction;
    }

    return sum;
}

static double twice(int count, double seed, ...)
{
    va_list arguments;
    va_list copy;
    va_start(arguments, seed);
    va_copy(copy, arguments);
    double first = consume(count, arguments);
    double second = consume(count, copy);
    va_end(copy);
    va_end(arguments);
    return seed + first + second;
}

static double exhausted(int a, int b, int c, int d, int e, int f, int g,
                        double p, double q, double r, double s, double t, double u, double v, double w, double x, ...)
{
    va_list arguments;
    va_start(arguments, x);
    int integer = va_arg(arguments, int);
    double fraction = va_arg(arguments, double);
    int last = va_arg(arguments, int);
    va_end(arguments);
    return a + b + c + d + e + f + g + p + q + r + s + t + u + v + w + x + integer + fraction + last;
}

static int forward(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int count = vprintf(format, arguments);
    va_end(arguments);
    return count;
}

int main(void)
{
    double a = twice(12, 0.25, 1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5,
                     7, 7.5, 8, 8.5, 9, 9.5, 10, 10.5, 11, 11.5, 12, 12.5);
    double b = exhausted(1, 2, 3, 4, 5, 6, 7, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 11, 12.5, 13);
    int length = forward("%.2f %.2f %s %d\n", a, b, "forwarded", 42);
    printf("length=%d\n", length);
    return 0;
}
