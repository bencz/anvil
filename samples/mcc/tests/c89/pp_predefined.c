/*
 * C89 Test: Predefined Macros
 * Tests __STDC__, __FILE__, __LINE__
 */

/* __STDC__ should be 1 for conforming implementations */
#ifdef __STDC__
int stdc_conforming = 1;
#else
int stdc_conforming = 0;
#endif

/* Test __LINE__ at different positions */
int line1 = __LINE__;
int line2 = __LINE__;
int line3 = __LINE__;

int main(void)
{
    const char *file;
    int line;
    
    /* Test __FILE__ and __LINE__ */
    file = __FILE__;
    line = __LINE__;
    
    /* Verify __LINE__ increases */
    if (line1 < line2 && line2 < line3) {
        return 0;
    }
    
    return 1;
}
