/*
 * Main test file - uses functions from math_funcs.c
 * Tests multi-file compilation support
 */

/* Declare external functions from math_funcs.c */
int add(int a, int b);
int subtract(int a, int b);
int multiply(int a, int b);
int square(int x);

int main(void)
{
    int a = 10;
    int b = 5;
    int result;
    
    result = add(a, b);        /* 15 */
    result = subtract(a, b);   /* 5 */
    result = multiply(a, b);   /* 50 */
    result = square(a);        /* 100 */
    
    /* Final result: 100 + 50 + 5 + 15 = 170 */
    result = add(result, multiply(a, b));
    result = add(result, subtract(a, b));
    result = add(result, add(a, b));
    
    return result;
}
