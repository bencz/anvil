/*
 * C99 Test: C99 Predefined Macros
 * Tests __STDC_VERSION__, __func__, and C99 specific macros
 */

/* C99 version check */
#if defined(__STDC_VERSION__)
    #if __STDC_VERSION__ >= 199901L
    int c99_version = 1;
    #else
    int c99_version = 0;
    #endif
#else
int c99_version = 0;
#endif

/* __STDC_HOSTED__ (C99) */
#ifdef __STDC_HOSTED__
int hosted = __STDC_HOSTED__;
#else
int hosted = 1;  /* Assume hosted */
#endif

/* Standard predefined macros */
const char *file = __FILE__;
int line = __LINE__;
const char *date = __DATE__;
const char *time_str = __TIME__;

/* Function to test __func__ */
const char *get_func_name(void)
{
    return __func__;
}

int check_func_name(void)
{
    const char *name = __func__;
    /* Should be "check_func_name" */
    return name[0] == 'c';
}

/* Nested function calls with __func__ */
const char *outer_func(void)
{
    return __func__;  /* "outer_func" */
}

const char *inner_func(void)
{
    return __func__;  /* "inner_func" */
}

int main(void)
{
    const char *main_name;
    const char *other_name;
    int result;
    
    /* __func__ in main */
    main_name = __func__;
    
    /* __func__ from other functions */
    other_name = get_func_name();
    
    /* Test function name checks */
    result = check_func_name();
    
    /* Get names from different functions */
    other_name = outer_func();
    other_name = inner_func();
    
    return c99_version;
}
