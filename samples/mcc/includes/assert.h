/*
 * MCC Standard Library - assert.h
 * Diagnostics
 */

#ifndef _ASSERT_H
#define _ASSERT_H

/* Assert function (to be provided by runtime) */
extern void __assert_fail(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? (void)0 : __assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif /* _ASSERT_H */
