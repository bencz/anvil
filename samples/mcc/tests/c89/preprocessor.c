/*
 * C89 Test: Preprocessor
 * Tests C89 preprocessor directives
 */

/* Object-like macros */
#define PI 3.14159
#define MAX_SIZE 100
#define EMPTY

/* Function-like macros */
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Macro with multiple statements */
#define SWAP(a, b, type) do { type temp = (a); (a) = (b); (b) = temp; } while(0)

/* Stringification */
#define STR(x) #x

/* Token pasting */
#define CONCAT(a, b) a##b

/* Conditional compilation */
#define DEBUG 1
#define VERSION 2

#if DEBUG
#define LOG(msg) /* debug logging */
#else
#define LOG(msg)
#endif

#ifdef DEBUG
int debug_mode = 1;
#else
int debug_mode = 0;
#endif

#ifndef RELEASE
int release_mode = 0;
#endif

/* Nested conditionals */
#if VERSION == 1
int version = 1;
#elif VERSION == 2
int version = 2;
#else
int version = 0;
#endif

/* Undef */
#define TEMP 100
#undef TEMP

/* Predefined macros */
const char *file = __FILE__;
int line = __LINE__;
const char *date = __DATE__;
const char *time_str = __TIME__;

/* Include guard pattern */
#ifndef MY_HEADER_H
#define MY_HEADER_H
/* header content */
#endif

int main(void)
{
    int x = 5, y = 10;
    int arr[MAX_SIZE];
    double area;
    int result;
    
    /* Use macros */
    area = PI * SQUARE(5);
    result = MAX(x, y);
    result = MIN(x, y);
    
    /* Swap using macro */
    SWAP(x, y, int);
    
    /* Array with macro size */
    arr[0] = 1;
    arr[MAX_SIZE - 1] = 100;
    
    LOG("test message");
    
    return 0;
}
