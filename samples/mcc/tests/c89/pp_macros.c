/*
 * C89 Test: Preprocessor Macros
 * Tests object-like and function-like macros
 */

/* Object-like macros */
#define NULL 0
#define TRUE 1
#define FALSE 0
#define PI 3.14159265358979
#define E 2.71828182845904
#define MAX_INT 2147483647
#define MIN_INT (-2147483647-1)

/* Empty macro */
#define EMPTY

/* Macro expanding to another macro */
#define BOOL int
#define BOOLEAN BOOL

/* Function-like macros */
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)

/* Macro with side-effect protection */
#define SQUARE(x) ((x) * (x))
#define CUBE(x) ((x) * (x) * (x))

/* Multi-statement macro */
#define SWAP(a, b, type) do { type _tmp = (a); (a) = (b); (b) = _tmp; } while(0)

/* Macro with no arguments but parentheses */
#define GET_ZERO() 0
#define GET_ONE() 1

/* Nested macro calls */
#define DOUBLE(x) ((x) + (x))
#define QUADRUPLE(x) DOUBLE(DOUBLE(x))

/* Macro in macro */
#define ADD(a, b) ((a) + (b))
#define ADD3(a, b, c) ADD(ADD(a, b), c)
#define ADD4(a, b, c, d) ADD(ADD(a, b), ADD(c, d))

int main(void)
{
    int x = 5, y = 10, z;
    int arr[10];
    double pi_squared;
    BOOLEAN flag;
    
    /* Test object-like macros */
    flag = TRUE;
    flag = FALSE;
    arr[0] = NULL;
    
    /* Test function-like macros */
    z = ABS(-5);
    z = MAX(x, y);
    z = MIN(x, y);
    z = CLAMP(15, 0, 10);
    
    /* Test arithmetic macros */
    z = SQUARE(x);
    z = CUBE(2);
    pi_squared = SQUARE(PI);
    
    /* Test swap macro */
    SWAP(x, y, int);
    
    /* Test nested macros */
    z = DOUBLE(5);
    z = QUADRUPLE(2);
    z = ADD3(1, 2, 3);
    z = ADD4(1, 2, 3, 4);
    
    /* Test macros with no args */
    z = GET_ZERO();
    z = GET_ONE();
    
    return 0;
}
