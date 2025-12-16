/*
 * C23 Test: C23 Preprocessor Features
 * Tests C23 specific preprocessor additions
 */

/* C23: #elifdef and #elifndef */
#define FEATURE_X

#ifdef FEATURE_X
int has_x = 1;
#elifdef FEATURE_Y
int has_y = 1;
#else
int has_neither = 1;
#endif

#ifndef FEATURE_Y
int no_y = 1;
#elifndef FEATURE_Z
int no_z = 1;
#else
int has_both = 1;
#endif

/* C23: #warning directive (standardized) */
/* #warning "This is a C23 warning" */

/* C23: __has_include (standardized from extensions) */
#if __has_include(<stdio.h>)
#define HAS_STDIO 1
#else
#define HAS_STDIO 0
#endif

/* C23: __has_c_attribute */
#if defined(__has_c_attribute)
    #if __has_c_attribute(nodiscard)
    #define HAS_NODISCARD 1
    #else
    #define HAS_NODISCARD 0
    #endif
#else
#define HAS_NODISCARD 0
#endif

/* C23: Binary literals in preprocessor */
#define BINARY_MASK 0b11110000
#define BINARY_VALUE 0b10101010

#if BINARY_MASK == 240
int binary_works = 1;
#else
int binary_works = 0;
#endif

/* C23: Digit separators in preprocessor */
#define LARGE_NUMBER 1'000'000
#define HEX_VALUE 0xFF'FF

#if LARGE_NUMBER == 1000000
int digit_sep_works = 1;
#else
int digit_sep_works = 0;
#endif

/* C23: __VA_OPT__ for variadic macros */
#define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)
#define CALL(f, ...) f(__VA_OPT__(__VA_ARGS__))

int printf(const char *fmt, ...);
void no_args(void);
void with_args(int a, int b);

int main(void)
{
    int x = BINARY_VALUE;
    int y = LARGE_NUMBER;
    
    /* Test __VA_OPT__ */
    LOG("no args");
    LOG("with args: %d", 42);
    
    CALL(no_args);
    CALL(with_args, 1, 2);
    
    return has_x + no_y + binary_works + digit_sep_works;
}
