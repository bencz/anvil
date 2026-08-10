#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

extern long long anvil_sum5(long long, long long, long long, long long, long long);
extern double anvil_fp_pressure(double, double, double, double);
extern long long anvil_call_variadic(void);

long long win_probe(long long tag, ...)
{
    va_list ap;
    va_start(ap, tag);
    double a = va_arg(ap, double);
    double b = va_arg(ap, double);
    double c = va_arg(ap, double);
    long long tail = va_arg(ap, long long);
    va_end(ap);
    return tag == 42 && a == 1.25 && b == -2.5 && c == 3.75 && tail == 99
        ? 777 : -1;
}

int main(void)
{
    if (anvil_sum5(1, 2, 3, 4, 5) != 15) return 10;
    if (anvil_call_variadic() != 777) return 11;
    uint64_t expected = UINT64_C(0x0123456789abcdef);
    uint64_t observed = 0;
    __asm__ volatile("movq %0, %%xmm6" : : "r"(expected) : "xmm6");
    double sum = anvil_fp_pressure(1.0, 2.0, 3.0, 4.0);
    __asm__ volatile("movq %%xmm6, %0" : "=r"(observed));
    if (sum != 10.0) return 12;
    if (observed != expected) return 13;
    puts("Win64 ABI execution conformance passed");
    return 0;
}
