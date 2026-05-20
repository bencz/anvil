#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int64_t anvil_rt_memory_select(int64_t idx,
                                      int64_t a,
                                      int64_t b,
                                      int8_t tiny,
                                      double fp);
extern int64_t anvil_rt_dynamic_alloca(int64_t count);
extern int64_t anvil_rt_global_plus(int64_t x);
extern const char *anvil_rt_string(void);
extern int8_t anvil_rt_signed_byte(void);
extern uint8_t anvil_rt_unsigned_byte(void);
extern int64_t anvil_rt_switch_pick(int64_t value);
extern int64_t anvil_rt_stack_sum10(int64_t a0,
                                    int64_t a1,
                                    int64_t a2,
                                    int64_t a3,
                                    int64_t a4,
                                    int64_t a5,
                                    int64_t a6,
                                    int64_t a7,
                                    int64_t a8,
                                    int64_t a9);
extern double anvil_rt_fp_stack_arg(double a0,
                                    double a1,
                                    double a2,
                                    double a3,
                                    double a4,
                                    double a5,
                                    double a6,
                                    double a7,
                                    double a8);
extern const char *anvil_rt_ptr_stack_arg(const char *p0,
                                          const char *p1,
                                          const char *p2,
                                          const char *p3,
                                          const char *p4,
                                          const char *p5,
                                          const char *p6,
                                          const char *p7,
                                          const char *p8);

static int failures = 0;

static void check_i64(const char *name, int64_t got, int64_t expected)
{
    if (got != expected) {
        printf("[FAIL] %s: got %lld, expected %lld\n",
               name, (long long)got, (long long)expected);
        failures++;
        return;
    }

    printf("[PASS] %s: %lld\n", name, (long long)got);
}

static void check_str(const char *name, const char *got, const char *expected)
{
    if (!got || strcmp(got, expected) != 0) {
        printf("[FAIL] %s: got %s, expected %s\n",
               name, got ? got : "(null)", expected);
        failures++;
        return;
    }

    printf("[PASS] %s: %s\n", name, got);
}

static void check_f64(const char *name, double got, double expected)
{
    if (got != expected) {
        printf("[FAIL] %s: got %.17g, expected %.17g\n",
               name, got, expected);
        failures++;
        return;
    }

    printf("[PASS] %s: %.17g\n", name, got);
}

int main(void)
{
    const char *stack_ptr = "stack-pointer";

    check_i64("memory/select/casts/fp",
              anvil_rt_memory_select(2, 40, 12, (int8_t)-3, -4.9),
              294);
    check_i64("dynamic alloca",
              anvil_rt_dynamic_alloca(3),
              77);
    check_i64("global load",
              anvil_rt_global_plus(9),
              50);
    check_str("string literal",
              anvil_rt_string(),
              "anvil-mir");
    check_i64("signed byte global",
              anvil_rt_signed_byte(),
              -5);
    check_i64("unsigned byte global",
              anvil_rt_unsigned_byte(),
              250);
    check_i64("switch case zero",
              anvil_rt_switch_pick(0),
              10);
    check_i64("switch case positive",
              anvil_rt_switch_pick(7),
              70);
    check_i64("switch case negative",
              anvil_rt_switch_pick(-3),
              33);
    check_i64("switch default",
              anvil_rt_switch_pick(99),
              -1);
    check_i64("incoming stack int args",
              anvil_rt_stack_sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10),
              55);
    check_f64("incoming stack fp arg",
              anvil_rt_fp_stack_arg(1.25, 2.0, 3.0, 4.0, 5.0,
                                    6.0, 7.0, 8.0, 8.75),
              10.0);
    check_str("incoming stack pointer arg",
              anvil_rt_ptr_stack_arg("p0", "p1", "p2", "p3", "p4",
                                     "p5", "p6", "p7", stack_ptr),
              stack_ptr);

    if (failures) {
        printf("%d basic runtime test(s) failed\n", failures);
        return 1;
    }

    printf("basic runtime tests passed\n");
    return 0;
}
