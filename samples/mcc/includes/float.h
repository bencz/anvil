/*
 * MCC Standard Library - float.h
 * Floating-point characteristics
 */

#ifndef _FLOAT_H
#define _FLOAT_H

/* Radix of exponent representation */
#define FLT_RADIX       2

/* Number of decimal digits of precision */
#define FLT_DIG         6
#define DBL_DIG         15
#define LDBL_DIG        15

/* Difference between 1.0 and the least value greater than 1.0 */
#define FLT_EPSILON     1.19209290e-07F
#define DBL_EPSILON     2.2204460492503131e-16
#define LDBL_EPSILON    2.2204460492503131e-16L

/* Number of FLT_RADIX digits in mantissa */
#define FLT_MANT_DIG    24
#define DBL_MANT_DIG    53
#define LDBL_MANT_DIG   53

/* Maximum representable finite value */
#define FLT_MAX         3.40282347e+38F
#define DBL_MAX         1.7976931348623157e+308
#define LDBL_MAX        1.7976931348623157e+308L

/* Maximum integer such that FLT_RADIX raised to that power minus 1 is representable */
#define FLT_MAX_EXP     128
#define DBL_MAX_EXP     1024
#define LDBL_MAX_EXP    1024

/* Maximum integer such that 10 raised to that power is representable */
#define FLT_MAX_10_EXP  38
#define DBL_MAX_10_EXP  308
#define LDBL_MAX_10_EXP 308

/* Minimum normalized positive value */
#define FLT_MIN         1.17549435e-38F
#define DBL_MIN         2.2250738585072014e-308
#define LDBL_MIN        2.2250738585072014e-308L

/* Minimum negative integer such that FLT_RADIX raised to that power minus 1 is normalized */
#define FLT_MIN_EXP     (-125)
#define DBL_MIN_EXP     (-1021)
#define LDBL_MIN_EXP    (-1021)

/* Minimum negative integer such that 10 raised to that power is normalized */
#define FLT_MIN_10_EXP  (-37)
#define DBL_MIN_10_EXP  (-307)
#define LDBL_MIN_10_EXP (-307)

/* Rounding mode */
#define FLT_ROUNDS      1   /* Round to nearest */

#endif /* _FLOAT_H */
