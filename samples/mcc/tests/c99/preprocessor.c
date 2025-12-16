/*
 * C99 Test: Preprocessor Features
 * Tests C99 preprocessor additions
 */

/* C99: Variadic macros */
#define PRINTF_LIKE(fmt, ...) printf(fmt, __VA_ARGS__)
#define DEBUG_LOG(fmt, ...) fprintf(stderr, "DEBUG: " fmt "\n", __VA_ARGS__)

/* C99: Empty macro arguments */
#define MACRO(a, b, c) a + b + c
#define RESULT MACRO(1, , 3)  /* Empty second argument */

/* C99: _Pragma operator */
#define DO_PRAGMA(x) _Pragma(#x)
#define DISABLE_WARNING(w) DO_PRAGMA(GCC diagnostic ignored #w)

/* C99: Predefined macros */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
int c99_mode = 1;
#else
int c99_mode = 0;
#endif

/* Macro with __VA_ARGS__ */
#define CALL(func, ...) func(__VA_ARGS__)

int add(int a, int b) { return a + b; }
int add3(int a, int b, int c) { return a + b + c; }

int main(void)
{
    int x = CALL(add, 1, 2);
    int y = CALL(add3, 1, 2, 3);
    
    return x + y;
}
