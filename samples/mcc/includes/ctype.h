/*
 * MCC Standard Library - ctype.h
 * Character handling
 */

#ifndef _CTYPE_H
#define _CTYPE_H

/* Character classification functions */
extern int isalnum(int c);
extern int isalpha(int c);
extern int iscntrl(int c);
extern int isdigit(int c);
extern int isgraph(int c);
extern int islower(int c);
extern int isprint(int c);
extern int ispunct(int c);
extern int isspace(int c);
extern int isupper(int c);
extern int isxdigit(int c);

/* Character conversion functions */
extern int tolower(int c);
extern int toupper(int c);

#endif /* _CTYPE_H */
