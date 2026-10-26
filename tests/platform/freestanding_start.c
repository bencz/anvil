/* Freestanding Linux entry point for cross-target execution tests run under
 * qemu-user: calls test_main() and exits with its result.  Also provides the
 * few C library routines a compiler may emit calls to. */
#include <stddef.h>

int test_main(void);

static void sys_exit(long code)
{
#if defined(__aarch64__)
    register long x0 __asm__("x0") = code;
    register long x8 __asm__("x8") = 93;
    __asm__ volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
#elif defined(__powerpc__)
    register long r0 __asm__("r0") = 1;
    register long r3 __asm__("r3") = code;
    __asm__ volatile("sc" : : "r"(r0), "r"(r3) : "memory", "cr0");
#else
#error "unsupported freestanding target"
#endif
    for (;;) {
    }
}

void _start(void)
{
    sys_exit(test_main());
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}
