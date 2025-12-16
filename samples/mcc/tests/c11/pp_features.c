/*
 * C11 Test: C11 Preprocessor Features
 * Tests C11 specific preprocessor additions
 */

/* _Pragma operator (C99/C11) */
#define STRINGIFY(x) #x
#define PRAGMA(x) _Pragma(STRINGIFY(x))

/* Diagnostic pragmas */
#define DISABLE_WARNING(w) PRAGMA(GCC diagnostic ignored STRINGIFY(w))
#define ENABLE_WARNING(w) PRAGMA(GCC diagnostic warning STRINGIFY(w))

/* Push/pop diagnostic state */
#define PUSH_WARNINGS PRAGMA(GCC diagnostic push)
#define POP_WARNINGS PRAGMA(GCC diagnostic pop)

/* C11: _Static_assert in preprocessor context */
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define COMPILE_TIME_ASSERT(cond) _Static_assert(cond, #cond)

/* Version-based feature selection */
#if __STDC_VERSION__ >= 201112L
#define HAS_STATIC_ASSERT 1
#define HAS_GENERIC 1
#define HAS_NORETURN 1
#define HAS_ALIGNAS 1
#else
#define HAS_STATIC_ASSERT 0
#define HAS_GENERIC 0
#define HAS_NORETURN 0
#define HAS_ALIGNAS 0
#endif

/* Feature test macros */
#if HAS_STATIC_ASSERT
_Static_assert(sizeof(int) >= 4, "int must be at least 4 bytes");
_Static_assert(sizeof(char) == 1, "char must be 1 byte");
#endif

/* Conditional compilation based on C11 features */
#if HAS_GENERIC
#define TYPE_NAME(x) _Generic((x), \
    int: "int", \
    float: "float", \
    double: "double", \
    default: "unknown")
#else
#define TYPE_NAME(x) "unknown"
#endif

int main(void)
{
    int x = 42;
    float f = 3.14f;
    
#if HAS_STATIC_ASSERT
    STATIC_ASSERT(sizeof(x) == sizeof(int), "size mismatch");
    COMPILE_TIME_ASSERT(sizeof(int) >= 2);
#endif

#if HAS_GENERIC
    const char *int_name = TYPE_NAME(x);
    const char *float_name = TYPE_NAME(f);
#endif
    
    return 0;
}
