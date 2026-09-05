/*
 * MCC Standard Library - stddef.h
 * Common definitions
 */

#ifndef _STDDEF_H
#define _STDDEF_H

/* NULL pointer constant */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* Size type */
typedef __SIZE_TYPE__ size_t;

/* Pointer difference type */
typedef __PTRDIFF_TYPE__ ptrdiff_t;

/* Wide character type */
typedef __WCHAR_TYPE__ wchar_t;

/* Offset of member in structure */
#define offsetof(type, member) ((size_t)&((type *)0)->member)

#endif /* _STDDEF_H */
