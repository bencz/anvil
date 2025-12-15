/*
 * MCC Standard Library - stdio.h
 * Standard input/output
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* EOF indicator */
#define EOF (-1)

/* Buffer sizes */
#define BUFSIZ      512
#define FILENAME_MAX 260
#define FOPEN_MAX   20
#define L_tmpnam    20
#define TMP_MAX     32767

/* Seek origins */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* Buffering modes */
#define _IOFBF      0   /* Full buffering */
#define _IOLBF      1   /* Line buffering */
#define _IONBF      2   /* No buffering */

/* File position type */
typedef long fpos_t;

/* FILE structure (opaque) */
typedef struct _FILE FILE;

/* Standard streams (to be provided by runtime) */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* File operations */
extern FILE *fopen(const char *filename, const char *mode);
extern FILE *freopen(const char *filename, const char *mode, FILE *stream);
extern int fclose(FILE *stream);
extern int fflush(FILE *stream);
extern void setbuf(FILE *stream, char *buf);
extern int setvbuf(FILE *stream, char *buf, int mode, size_t size);

/* Formatted input/output */
extern int fprintf(FILE *stream, const char *format, ...);
extern int printf(const char *format, ...);
extern int sprintf(char *str, const char *format, ...);
extern int vfprintf(FILE *stream, const char *format, va_list ap);
extern int vprintf(const char *format, va_list ap);
extern int vsprintf(char *str, const char *format, va_list ap);

extern int fscanf(FILE *stream, const char *format, ...);
extern int scanf(const char *format, ...);
extern int sscanf(const char *str, const char *format, ...);

/* Character input/output */
extern int fgetc(FILE *stream);
extern char *fgets(char *str, int n, FILE *stream);
extern int fputc(int c, FILE *stream);
extern int fputs(const char *str, FILE *stream);
extern int getc(FILE *stream);
extern int getchar(void);
extern char *gets(char *str);
extern int putc(int c, FILE *stream);
extern int putchar(int c);
extern int puts(const char *str);
extern int ungetc(int c, FILE *stream);

/* Direct input/output */
extern size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* File positioning */
extern int fgetpos(FILE *stream, fpos_t *pos);
extern int fseek(FILE *stream, long offset, int whence);
extern int fsetpos(FILE *stream, const fpos_t *pos);
extern long ftell(FILE *stream);
extern void rewind(FILE *stream);

/* Error handling */
extern void clearerr(FILE *stream);
extern int feof(FILE *stream);
extern int ferror(FILE *stream);
extern void perror(const char *str);

/* File removal and renaming */
extern int remove(const char *filename);
extern int rename(const char *oldname, const char *newname);

/* Temporary files */
extern FILE *tmpfile(void);
extern char *tmpnam(char *str);

#endif /* _STDIO_H */
