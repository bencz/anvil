/*
 * MCC Standard Library - stdarg.h
 * Variable argument handling
 */

#ifndef _STDARG_H
#define _STDARG_H

/* Variable argument list type */
typedef char *va_list;

/* Size of argument rounded up to int boundary */
#define _VA_SIZE(type) (((sizeof(type) + sizeof(int) - 1) / sizeof(int)) * sizeof(int))

/* Initialize va_list */
#define va_start(ap, last) ((ap) = (va_list)&(last) + _VA_SIZE(last))

/* Get next argument */
#define va_arg(ap, type) (*(type *)((ap) += _VA_SIZE(type), (ap) - _VA_SIZE(type)))

/* End variable argument processing */
#define va_end(ap) ((ap) = (va_list)0)

/* Copy va_list */
#define va_copy(dest, src) ((dest) = (src))

#endif /* _STDARG_H */
