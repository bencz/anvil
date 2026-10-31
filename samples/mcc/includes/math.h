/*
 * MCC Standard Library - math.h
 * Mathematics
 */

#ifndef _MATH_H
#define _MATH_H

/* Huge value (infinity) */
#define HUGE_VAL    1e308

/* Trigonometric functions */
extern double acos(double x);
extern double asin(double x);
extern double atan(double x);
extern double atan2(double y, double x);
extern double cos(double x);
extern double sin(double x);
extern double tan(double x);

/* Hyperbolic functions */
extern double cosh(double x);
extern double sinh(double x);
extern double tanh(double x);

/* Exponential and logarithmic functions */
extern double exp(double x);
extern double frexp(double x, int *exp);
extern double ldexp(double x, int exp);
extern double log(double x);
extern double log10(double x);
extern double modf(double x, double *iptr);

/* Power functions */
extern double pow(double x, double y);
extern double sqrt(double x);

/* Nearest integer, absolute value, and remainder functions */
extern double ceil(double x);
extern double fabs(double x);
extern double floor(double x);
extern double fmod(double x, double y);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
/* C99: errors are reported through errno. */
#define MATH_ERRNO          1
#define MATH_ERREXCEPT      2
#define math_errhandling    MATH_ERRNO

/* C99 float forms */
extern float acosf(float x);
extern float asinf(float x);
extern float atanf(float x);
extern float atan2f(float y, float x);
extern float cosf(float x);
extern float sinf(float x);
extern float tanf(float x);
extern float expf(float x);
extern float logf(float x);
extern float powf(float x, float y);
extern float sqrtf(float x);
extern float ceilf(float x);
extern float fabsf(float x);
extern float floorf(float x);
extern float fmodf(float x, float y);
#endif

#endif /* _MATH_H */
