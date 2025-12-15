/*
 * MCC Standard Library - string.h
 * String handling
 */

#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

/* Copying functions */
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memmove(void *dest, const void *src, size_t n);
extern char *strcpy(char *dest, const char *src);
extern char *strncpy(char *dest, const char *src, size_t n);

/* Concatenation functions */
extern char *strcat(char *dest, const char *src);
extern char *strncat(char *dest, const char *src, size_t n);

/* Comparison functions */
extern int memcmp(const void *s1, const void *s2, size_t n);
extern int strcmp(const char *s1, const char *s2);
extern int strcoll(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, size_t n);
extern size_t strxfrm(char *dest, const char *src, size_t n);

/* Search functions */
extern void *memchr(const void *s, int c, size_t n);
extern char *strchr(const char *s, int c);
extern size_t strcspn(const char *s1, const char *s2);
extern char *strpbrk(const char *s1, const char *s2);
extern char *strrchr(const char *s, int c);
extern size_t strspn(const char *s1, const char *s2);
extern char *strstr(const char *s1, const char *s2);
extern char *strtok(char *s1, const char *s2);

/* Miscellaneous functions */
extern void *memset(void *s, int c, size_t n);
extern char *strerror(int errnum);
extern size_t strlen(const char *s);

#endif /* _STRING_H */
