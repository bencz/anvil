#ifndef ANVIL_TEST_SCALAR_VARIADIC_H
#define ANVIL_TEST_SCALAR_VARIADIC_H

#include <stdarg.h>

typedef struct {
    char prefix;
    va_list cursor;
    char suffix;
} abi_cursor_record;

unsigned abi_cursor_size(void);
unsigned abi_cursor_offset(void);
unsigned abi_cursor_record_size(void);
int abi_cursor_next(va_list *cursor);

double abi_variadic_sum(int count, double seed, ...);
double abi_variadic_exhausted(int a, int b, int c, int d, int e, int f, int g,
                             double p, double q, double r, double s, double t, double u, double v, double w, double x, ...);
int abi_variadic_forward(const char *format, ...);

#endif
