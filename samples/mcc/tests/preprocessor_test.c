/*
 * Preprocessor test for MCC
 * Tests: #define, #ifdef, #ifndef, #if, #elif, #else, #endif
 */

/* Test 1: Simple object-like macros */
#define VERSION 100
#define PI_APPROX 3

/* Test 2: Function-like macros */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SQUARE(x) ((x) * (x))
#define CUBE(x) ((x) * (x) * (x))

/* Test 3: Nested macro expansion */
#define DOUBLE(x) ((x) + (x))

/* Test 4: Conditional compilation - #ifdef/#ifndef */
#define DEBUG_MODE

#ifdef DEBUG_MODE
#define DEBUG_VALUE 1
#else
#define DEBUG_VALUE 0
#endif

#ifndef RELEASE_MODE
#define RELEASE_VALUE 0
#else
#define RELEASE_VALUE 1
#endif

/* Test 5: Conditional compilation - #if/#elif/#else */
#define PLATFORM 1

#if PLATFORM == 0
#define PLATFORM_NAME 0
#elif PLATFORM == 1
#define PLATFORM_NAME 1
#elif PLATFORM == 2
#define PLATFORM_NAME 2
#else
#define PLATFORM_NAME 99
#endif

/* Test 6: Arithmetic in #if */
#define A 5
#define B 3

#if A + B > 7
#define SUM_CHECK 1
#else
#define SUM_CHECK 0
#endif

#if A * B == 15
#define MUL_CHECK 1
#else
#define MUL_CHECK 0
#endif

/* Test 7: Logical operators in #if */
#define X 1
#define Y 0

#if X && Y
#define AND_RESULT 1
#else
#define AND_RESULT 0
#endif

#if X || Y
#define OR_RESULT 1
#else
#define OR_RESULT 0
#endif

#if !Y
#define NOT_RESULT 1
#else
#define NOT_RESULT 0
#endif

/* Test 8: defined() operator */
#if defined(DEBUG_MODE)
#define DEFINED_CHECK 1
#else
#define DEFINED_CHECK 0
#endif

#if !defined(UNDEFINED_MACRO)
#define UNDEFINED_CHECK 1
#else
#define UNDEFINED_CHECK 0
#endif

/* Test functions using macros */
int test_object_macros(void)
{
    return VERSION + PI_APPROX;
}

int test_function_macros(int a, int b)
{
    int max_val = MAX(a, b);
    int min_val = MIN(a, b);
    int sq = SQUARE(a);
    int cb = CUBE(b);
    return max_val + min_val + sq + cb;
}

int test_double(int x)
{
    return DOUBLE(x);
}

int test_conditionals(void)
{
    int result = 0;
    
    result = result + DEBUG_VALUE;
    result = result + RELEASE_VALUE;
    result = result + PLATFORM_NAME;
    result = result + SUM_CHECK;
    result = result + MUL_CHECK;
    result = result + AND_RESULT;
    result = result + OR_RESULT;
    result = result + NOT_RESULT;
    result = result + DEFINED_CHECK;
    result = result + UNDEFINED_CHECK;
    
    return result;
}

int main(void)
{
    int result = 0;
    
    /* Test object-like macros: VERSION(100) + PI_APPROX(3) = 103 */
    result = result + test_object_macros();
    
    /* Test function-like macros with 5, 3:
     * MAX(5,3)=5, MIN(5,3)=3, SQUARE(5)=25, CUBE(3)=27
     * Total: 5+3+25+27 = 60
     */
    result = result + test_function_macros(5, 3);
    
    /* Test DOUBLE: DOUBLE(2) = 4 */
    result = result + test_double(2);
    
    /* Test conditionals:
     * DEBUG_VALUE=1, RELEASE_VALUE=0, PLATFORM_NAME=1
     * SUM_CHECK=1, MUL_CHECK=1
     * AND_RESULT=0, OR_RESULT=1, NOT_RESULT=1
     * DEFINED_CHECK=1, UNDEFINED_CHECK=1
     * Total: 1+0+1+1+1+0+1+1+1+1 = 8
     */
    result = result + test_conditionals();
    
    /* Expected total: 103 + 60 + 4 + 8 = 175 */
    return result;
}
