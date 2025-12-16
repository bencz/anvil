/*
 * C11 Test: C11 Predefined Macros
 * Tests __STDC_VERSION__ for C11 and C11 specific features
 */

/* C11 version check */
#if defined(__STDC_VERSION__)
    #if __STDC_VERSION__ >= 201112L
    int c11_version = 1;
    #else
    int c11_version = 0;
    #endif
#else
int c11_version = 0;
#endif

/* __STDC_NO_* macros (C11) - optional features */
#ifdef __STDC_NO_ATOMICS__
int no_atomics = 1;
#else
int no_atomics = 0;
#endif

#ifdef __STDC_NO_COMPLEX__
int no_complex = 1;
#else
int no_complex = 0;
#endif

#ifdef __STDC_NO_THREADS__
int no_threads = 1;
#else
int no_threads = 0;
#endif

#ifdef __STDC_NO_VLA__
int no_vla = 1;
#else
int no_vla = 0;
#endif

/* __STDC_UTF_16__ and __STDC_UTF_32__ (C11) */
#ifdef __STDC_UTF_16__
int utf16_support = 1;
#else
int utf16_support = 0;
#endif

#ifdef __STDC_UTF_32__
int utf32_support = 1;
#else
int utf32_support = 0;
#endif

/* Standard predefined macros */
const char *file = __FILE__;
int line = __LINE__;
const char *date = __DATE__;
const char *time_str = __TIME__;

/* __func__ (also in C99) */
const char *get_func(void)
{
    return __func__;
}

int main(void)
{
    const char *name = __func__;
    
    return c11_version;
}
