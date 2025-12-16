/*
 * C99 Test: Variadic Macros
 * Tests __VA_ARGS__ and variadic macro features
 */

/* Basic variadic macro */
#define PRINT_ARGS(...) printf(__VA_ARGS__)

/* Variadic with fixed args */
#define LOG(level, fmt, ...) printf("[%s] " fmt "\n", level, __VA_ARGS__)
#define DEBUG(fmt, ...) LOG("DEBUG", fmt, __VA_ARGS__)
#define INFO(fmt, ...) LOG("INFO", fmt, __VA_ARGS__)
#define ERROR(fmt, ...) LOG("ERROR", fmt, __VA_ARGS__)

/* Variadic macro forwarding */
#define FORWARD(...) other_func(__VA_ARGS__)
#define CALL_WITH_ARGS(func, ...) func(__VA_ARGS__)

/* Count arguments (limited) */
#define COUNT_ARGS_IMPL(_1, _2, _3, _4, _5, N, ...) N
#define COUNT_ARGS(...) COUNT_ARGS_IMPL(__VA_ARGS__, 5, 4, 3, 2, 1)

/* Variadic with empty args (C99 extension) */
#define MAYBE_ARGS(...) func(__VA_ARGS__)

/* Macro that uses variadic in expression */
#define SUM2(a, b) ((a) + (b))
#define SUM3(a, b, c) ((a) + (b) + (c))
#define SUM(...) sum_impl(__VA_ARGS__)

int printf(const char *fmt, ...);
void other_func(int a, int b, int c);

int sum_impl(int a, int b) { return a + b; }
int sum3_impl(int a, int b, int c) { return a + b + c; }

int add(int a, int b) { return a + b; }
int add3(int a, int b, int c) { return a + b + c; }

int main(void)
{
    int x, y, z;
    
    /* Test basic variadic */
    PRINT_ARGS("Hello %s\n", "world");
    
    /* Test with fixed args */
    DEBUG("value = %d", 42);
    INFO("status = %s", "ok");
    ERROR("code = %d", -1);
    
    /* Test forwarding */
    CALL_WITH_ARGS(add, 1, 2);
    CALL_WITH_ARGS(add3, 1, 2, 3);
    
    /* Test in expressions */
    x = SUM2(1, 2);
    y = SUM3(1, 2, 3);
    z = SUM(10, 20);
    
    return x + y + z;
}
