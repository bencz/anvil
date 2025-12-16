/*
 * C99 Test: Line Comments
 * Tests C99 single-line comments
 */

// This is a C99 line comment
int x = 10; // Comment at end of line

// Multiple
// line
// comments

int main(void)
{
    int a = 5; // inline comment
    
    // Comment before statement
    int b = 10;
    
    /* Block comment still works */
    int c = a + b;
    
    // Nested /* block */ in line comment
    int d = c * 2;
    
    return d;
}
