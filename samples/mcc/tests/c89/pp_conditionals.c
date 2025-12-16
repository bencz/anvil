/*
 * C89 Test: Preprocessor Conditionals
 * Tests #if, #ifdef, #ifndef, #elif, #else, #endif
 */

/* Basic #if with constant */
#define VERSION 3
#define MAJOR 2
#define MINOR 1

#if VERSION > 2
int version_ok = 1;
#else
int version_ok = 0;
#endif

/* #if with arithmetic */
#if (MAJOR * 100 + MINOR) >= 201
int version_check = 1;
#else
int version_check = 0;
#endif

/* #ifdef / #ifndef */
#define FEATURE_A
#undef FEATURE_B

#ifdef FEATURE_A
int has_feature_a = 1;
#else
int has_feature_a = 0;
#endif

#ifndef FEATURE_B
int no_feature_b = 1;
#else
int no_feature_b = 0;
#endif

/* Nested conditionals */
#define PLATFORM 1
#define DEBUG 1

#if PLATFORM == 1
    #if DEBUG
    int platform1_debug = 1;
    #else
    int platform1_release = 1;
    #endif
#elif PLATFORM == 2
    #if DEBUG
    int platform2_debug = 1;
    #else
    int platform2_release = 1;
    #endif
#else
    int unknown_platform = 1;
#endif

/* #elif chain */
#define MODE 2

#if MODE == 0
int mode_zero = 1;
#elif MODE == 1
int mode_one = 1;
#elif MODE == 2
int mode_two = 1;
#elif MODE == 3
int mode_three = 1;
#else
int mode_other = 1;
#endif

/* Logical operators in #if */
#define A 1
#define B 0
#define C 1

#if A
#if B
int a_and_b = 1;
#else
int a_and_b = 0;
#endif
#else
int a_and_b = 0;
#endif

#if A
int a_or_b = 1;
#elif B
int a_or_b = 1;
#else
int a_or_b = 0;
#endif

#if B
int not_b = 0;
#else
int not_b = 1;
#endif

#if A
int complex_cond = 1;
#elif B
int complex_cond = 1;
#else
int complex_cond = 0;
#endif

/* defined() operator */
#ifdef FEATURE_A
int defined_a = 1;
#else
int defined_a = 0;
#endif

#ifdef FEATURE_A
#ifndef FEATURE_B
int a_not_b = 1;
#else
int a_not_b = 0;
#endif
#else
int a_not_b = 0;
#endif

/* Comparison operators */
#define X 10
#define Y 20

#if X == 10
int x_eq_10 = 1;
#endif

#if X != Y
int x_ne_y = 1;
#endif

#if X < Y
int x_lt_y = 1;
#endif

#if Y > X
int y_gt_x = 1;
#endif

#if X <= 10
int x_le_10 = 1;
#endif

#if Y >= 20
int y_ge_20 = 1;
#endif

int main(void)
{
    return version_ok + version_check + has_feature_a + no_feature_b;
}
