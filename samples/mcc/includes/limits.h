/*
 * MCC Standard Library - limits.h
 * Implementation limits
 */

#ifndef _LIMITS_H
#define _LIMITS_H

/* Number of bits in a char */
#define CHAR_BIT    8

/* Minimum and maximum values for signed char */
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127

/* Maximum value for unsigned char */
#define UCHAR_MAX   255

/* Minimum and maximum values for char */
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

/* Minimum and maximum values for short */
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767

/* Maximum value for unsigned short */
#define USHRT_MAX   65535

/* Minimum and maximum values for int */
#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647

/* Maximum value for unsigned int */
#define UINT_MAX    4294967295U

/* Minimum and maximum values for long */
#define LONG_MIN    (-2147483647L - 1)
#define LONG_MAX    2147483647L

/* Maximum value for unsigned long */
#define ULONG_MAX   4294967295UL

/* Minimum and maximum values for multibyte character */
#define MB_LEN_MAX  1

#endif /* _LIMITS_H */
