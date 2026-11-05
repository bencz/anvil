/* What the drivers need from the C library, declared here so that they
 * build hosted and freestanding (qemu/, with BENCH_SMALL: the reduced
 * workloads for instruction counting under qemu-user). */
#ifndef ANVIL_BENCH_DRIVER_H
#define ANVIL_BENCH_DRIVER_H

#include <stddef.h>
#include <stdint.h>

int printf(const char *format, ...);
void *memset(void *dst, int value, size_t size);

#ifdef BENCH_SMALL
#define BENCH_PICK(full, small) (small)
#else
#define BENCH_PICK(full, small) (full)
#endif

#endif
