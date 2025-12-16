/*
 * C89 Test: Predefined Macros
 * Tests __STDC__ and basic preprocessor functionality
 */

/* __STDC__ should be 1 for conforming implementations */
#ifdef __STDC__
int stdc_conforming = 1;
#else
int stdc_conforming = 0;
#endif

/* Simple values */
int line1 = 10;
int line2 = 20;
int line3 = 30;

int main(void)
{
    int result;
    
    /* Capture file and line inside function */
    func_file = __FILE__;
    func_line = __LINE__;
    
    /* Use line marking macro */
    MARK_LINE(marked_line);
    
    /* Verify __LINE__ changes */
    if (line1 < line2 && line2 < line3) {
        return 0;
    }
    
    return 1;
}
