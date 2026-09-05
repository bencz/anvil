#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern long long anvil_sum5(long long, long long, long long, long long, long long);
extern double anvil_fp_pressure(double, double, double, double);
extern long long anvil_call_variadic(void);
extern long long anvil_dynamic_buffer(long long);
extern long long anvil_large_buffer(long long);
extern double anvil_mixed(long long, double, long long, double, double, long long, double);
extern long long anvil_unwind(long long, long long (*)(void));
extern double anvil_negative_abs_a(double);
extern double anvil_negative_abs_b(double);
extern long long anvil_store_before_trap(long long *, long long);

#ifdef _WIN32
static DWORD WINAPI stack_thread(void *arg)
{
    (void)arg;
    if (anvil_large_buffer(0) != 123)
        return 1;
    if (anvil_dynamic_buffer(16384) != 123)
        return 2;
    return 0;
}
#ifdef _MSC_VER
static volatile long long observed_before_trap;

static int check_store_before_trap(void)
{
    observed_before_trap = 0;
    __try
    {
        anvil_store_before_trap((long long *)&observed_before_trap, 0);
    }
    __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return observed_before_trap == 1;
    }

    return 0;
}

static long long raise_from_generated_frame(void)
{
    RaiseException(0xe0424242, 0, 0, NULL);
    return -1;
}

static int check_unwind(void)
{
    __try
    {
        anvil_unwind(16384, raise_from_generated_frame);
    }
    __except (GetExceptionCode() == 0xe0424242 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return 1;
    }
    return 0;
}
#endif
#endif

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
    if (anvil_negative_abs_a(3.5) != -3.5 || anvil_negative_abs_b(-2.25) != -2.25)
        return 21;

    if (anvil_dynamic_buffer(64) != 123)
        return 14;
    if (anvil_large_buffer(0) != 123)
        return 15;
    if (anvil_mixed(1, 2.0, 3, 4.0, 5.0, 6, 7.0) != 28.0)
        return 16;
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, stack_thread, NULL, 0, NULL);
    DWORD status = 1;
    if (!thread)
        return 17;
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &status))
        return 18;
    CloseHandle(thread);
    if (status)
        return 19;
#ifdef _MSC_VER
    if (!check_unwind())
        return 20;
    if (!check_store_before_trap())
        return 22;
#endif
#endif
    if (anvil_sum5(1, 2, 3, 4, 5) != 15) return 10;
    if (anvil_call_variadic() != 777) return 11;
    uint64_t expected[2] = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
    uint64_t observed[2] = {0, 0};
    __asm__ volatile("movdqu %0, %%xmm6" : : "m"(expected) : "xmm6");
    double sum = anvil_fp_pressure(1.0, 2.0, 3.0, 4.0);
    __asm__ volatile("movdqu %%xmm6, %0" : "=m"(observed));
    if (sum != 10.0) return 12;
    if (observed[0] != expected[0] || observed[1] != expected[1])
        return 13;
    puts("Win64 ABI execution conformance passed");
    return 0;
}
