/*
 * C23 Test: C23 Predefined Macros
 * Tests __STDC_VERSION__ for C23 and C23 specific features
 */

/* C23 version check */
#if defined(__STDC_VERSION__)
    #if __STDC_VERSION__ >= 202311L
    int c23_version = 1;
    #else
    int c23_version = 0;
    #endif
#else
int c23_version = 0;
#endif

/* Standard predefined macros */
const char *file = __FILE__;
int line = __LINE__;
const char *date = __DATE__;
const char *time_str = __TIME__;

/* __func__ */
const char *get_func(void)
{
    return __func__;
}

/* C23: true and false are keywords */
int bool_true = true;
int bool_false = false;

/* C23: nullptr */
void *null_ptr = nullptr;

int main(void)
{
    const char *name = __func__;
    int b = true;
    void *p = nullptr;
    
    return c23_version;
}
