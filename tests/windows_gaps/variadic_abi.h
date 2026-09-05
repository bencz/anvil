#ifndef ANVIL_TEST_VARIADIC_ABI_H
#define ANVIL_TEST_VARIADIC_ABI_H

#include "aggregate_abi.h"

double variadic_mixed(int count, ...);
long long variadic_stack(int a, int b, int c, int d, int e, ...);
struct Large variadic_result(int tag, ...);
double variadic_float(double first, ...);
int variadic_vla(int count, ...);

#endif
