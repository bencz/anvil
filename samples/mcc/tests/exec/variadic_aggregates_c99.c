/* Aggregates passed by value to a variadic function and read back with va_arg,
 * first in registers, then (after many scalars) on the stack. */
#include <stdarg.h>
#include <stdio.h>
struct S8 { int a, b; };
struct S16 { long long a, b; };
struct SM { double x; long long y; };
struct H3 { double x, y, z; };
struct F2 { float a, b; };
struct B5 { long long v[5]; };
struct C3 { char c[3]; };
long long consume(int pad, ...)
{
    va_list ap;
    va_start(ap, pad);
    long long h = 7;
    for (int i = 0; i < pad; i++) h = h * 31 + va_arg(ap, int);
    for (int i = 0; i < pad; i++) h = h * 31 + (long long)(va_arg(ap, double) * 4);
    struct S8 s8 = va_arg(ap, struct S8); h = h * 31 + s8.a * 3 + s8.b;
    struct S16 s16 = va_arg(ap, struct S16); h = h * 31 + s16.a * 5 + s16.b;
    struct SM sm = va_arg(ap, struct SM); h = h * 31 + (long long)(sm.x * 8) + sm.y;
    struct H3 h3 = va_arg(ap, struct H3); h = h * 31 + (long long)(h3.x * 2 + h3.y * 3 + h3.z * 5);
    struct F2 f2 = va_arg(ap, struct F2); h = h * 31 + (long long)(f2.a * 4 + f2.b * 16);
    struct B5 b5 = va_arg(ap, struct B5); for (int i = 0; i < 5; i++) h = h * 31 + b5.v[i];
    struct C3 c3 = va_arg(ap, struct C3); h = h * 31 + c3.c[0] * 100 + c3.c[1] * 10 + c3.c[2];
    h = h * 31 + va_arg(ap, int);
    va_end(ap);
    return h;
}
int main(void)
{
    struct S8 s8 = {3, 4}; struct S16 s16 = {5, -6}; struct SM sm = {1.5, 9}; struct H3 h3 = {1.0, 2.0, 3.5};
    struct F2 f2 = {0.25f, 2.0f}; struct B5 b5 = {{11, 12, 13, 14, 15}}; struct C3 c3 = {{1, 2, 3}};
    long long a = consume(0, s8, s16, sm, h3, f2, b5, c3, 77);
    long long b = consume(8, 1, 2, 3, 4, 5, 6, 7, 8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, s8, s16, sm, h3, f2, b5, c3, 99);
    printf("%lld %lld\n", a, b);
    return 0;
}
