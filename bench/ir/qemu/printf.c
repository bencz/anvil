/* Freestanding printf for the benchmark drivers under qemu-user: %d %u %x
 * %s %c with l/ll, zero padding and widths, buffered and written at exit. */
#include <stdarg.h>
#include <stddef.h>
static long sys_write(const char *buf, long len)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = 1;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = len;
    register long x8 __asm__("x8") = 64;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
#else
    register long r0 __asm__("r0") = 4;
    register long r3 __asm__("r3") = 1;
    register long r4 __asm__("r4") = (long)buf;
    register long r5 __asm__("r5") = len;
    __asm__ volatile("sc" : "+r"(r3) : "r"(r0), "r"(r4), "r"(r5) : "memory", "cr0");
    return r3;
#endif
}
static char out[4096]; static int pos;
static void put(char c) { if (pos < (int)sizeof(out)) out[pos++] = c; }
static void put_u(unsigned long long v, int base, int width, char pad)
{
    char tmp[32]; int n = 0;
    do { int d = (int)(v % base); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    while (n < width) { put(pad); width--; }
    while (n) put(tmp[--n]);
}
int vprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(*fmt); continue; }
        fmt++;
        char pad = ' '; int width = 0, longs = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { longs++; fmt++; }
        if (*fmt == 'd') { long long v = longs == 2 ? va_arg(ap, long long) : longs == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { put('-'); put_u(0ULL - (unsigned long long)v, 10, width, pad); } else put_u((unsigned long long)v, 10, width, pad); }
        else if (*fmt == 'u') put_u(longs == 2 ? va_arg(ap, unsigned long long) : longs == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, width, pad);
        else if (*fmt == 'x') put_u(longs == 2 ? va_arg(ap, unsigned long long) : longs == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, width, pad);
        else if (*fmt == 's') { const char *s = va_arg(ap, const char *); while (*s) put(*s++); }
        else if (*fmt == 'c') put((char)va_arg(ap, int));
        else put(*fmt);
    }
    return 0;
}
int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
int main(void);
int test_main(void) { int r = main(); sys_write(out, pos); return r; }
