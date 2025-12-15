/*
 * MCC Standard Library - stdlib.h
 * General utilities
 */

#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

/* Exit status codes */
#define EXIT_SUCCESS    0
#define EXIT_FAILURE    1

/* Maximum value returned by rand() */
#define RAND_MAX        32767

/* Division result types */
typedef struct {
    int quot;   /* Quotient */
    int rem;    /* Remainder */
} div_t;

typedef struct {
    long quot;  /* Quotient */
    long rem;   /* Remainder */
} ldiv_t;

/* String conversion functions */
extern double atof(const char *str);
extern int atoi(const char *str);
extern long atol(const char *str);
extern double strtod(const char *str, char **endptr);
extern long strtol(const char *str, char **endptr, int base);
extern unsigned long strtoul(const char *str, char **endptr, int base);

/* Pseudo-random number generation */
extern int rand(void);
extern void srand(unsigned int seed);

/* Memory management */
extern void *malloc(size_t size);
extern void *calloc(size_t nmemb, size_t size);
extern void *realloc(void *ptr, size_t size);
extern void free(void *ptr);

/* Communication with the environment */
extern void abort(void);
extern int atexit(void (*func)(void));
extern void exit(int status);
extern char *getenv(const char *name);
extern int system(const char *command);

/* Searching and sorting */
extern void *bsearch(const void *key, const void *base, size_t nmemb,
                     size_t size, int (*compar)(const void *, const void *));
extern void qsort(void *base, size_t nmemb, size_t size,
                  int (*compar)(const void *, const void *));

/* Integer arithmetic */
extern int abs(int n);
extern long labs(long n);
extern div_t div(int numer, int denom);
extern ldiv_t ldiv(long numer, long denom);

/* Multibyte/wide character conversion */
extern int mblen(const char *s, size_t n);
extern int mbtowc(wchar_t *pwc, const char *s, size_t n);
extern int wctomb(char *s, wchar_t wc);
extern size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n);
extern size_t wcstombs(char *s, const wchar_t *pwcs, size_t n);

#endif /* _STDLIB_H */
