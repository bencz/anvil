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

#endif /* _MATH_H */
