/*
 * complex.h - Complex number arithmetic
 * MCC Standard Library Header (C99)
 */

#ifndef _COMPLEX_H
#define _COMPLEX_H

/* Complex type macros */
#define complex         _Complex
#define _Complex_I      (0.0 + 1.0i)
#define I               _Complex_I

/* Type aliases for function declarations */
typedef double _Complex double_complex;
typedef float _Complex float_complex;
typedef long double _Complex ldouble_complex;

/* Double complex functions - declared as external */
double creal(double_complex z);
double cimag(double_complex z);
double cabs(double_complex z);
double carg(double_complex z);
double_complex conj(double_complex z);
double_complex cproj(double_complex z);

/* Float complex functions */
float crealf(float_complex z);
float cimagf(float_complex z);
float cabsf(float_complex z);
float cargf(float_complex z);
float_complex conjf(float_complex z);
float_complex cprojf(float_complex z);

/* Long double complex functions */
long double creall(ldouble_complex z);
long double cimagl(ldouble_complex z);
long double cabsl(ldouble_complex z);
long double cargl(ldouble_complex z);
ldouble_complex conjl(ldouble_complex z);
ldouble_complex cprojl(ldouble_complex z);

/* Exponential and logarithmic functions */
double_complex cexp(double_complex z);
double_complex clog(double_complex z);

/* Power functions */
double_complex cpow(double_complex x, double_complex y);
double_complex csqrt(double_complex z);

/* Trigonometric functions */
double_complex csin(double_complex z);
double_complex ccos(double_complex z);
double_complex ctan(double_complex z);
double_complex casin(double_complex z);
double_complex cacos(double_complex z);
double_complex catan(double_complex z);

/* Hyperbolic functions */
double_complex csinh(double_complex z);
double_complex ccosh(double_complex z);
double_complex ctanh(double_complex z);
double_complex casinh(double_complex z);
double_complex cacosh(double_complex z);
double_complex catanh(double_complex z);

#endif /* _COMPLEX_H */
