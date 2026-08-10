/*
 * C11 Test: New Keywords
 * Tests C11 new keywords and features
 */

/* C11: _Alignof operator */
int align_of_int = _Alignof(int);
int align_of_double = _Alignof(double);

/* C11: _Static_assert */
_Static_assert(sizeof(int) >= 4, "int must be at least 4 bytes");
_Static_assert(sizeof(long) >= sizeof(int), "long must be >= int");

int main(void)
{
    /* _Alignof in expression */
    int a = _Alignof(int);
    int b = _Alignof(double);
    int c = _Alignof(char);
    
    /* _Static_assert in block scope */
    _Static_assert(sizeof(char) == 1, "char must be 1 byte");
    
    return a + b + c;
}
