#ifndef ST_FATAL_H
#define ST_FATAL_H

/* Unrecoverable runtime failures: one diagnostic line on stderr, then
 * abort().  Header-only (standard C only), so any runtime object can report
 * without extra link dependencies, and allocation-free beyond stdio. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline _Noreturn void st_fatal(const char *component, const char *detail)
{
    fprintf(stderr, "smalltalk runtime: fatal: %s%s%s\n", component ? component : "runtime",
            detail && detail[0] ? ": " : "", detail ? detail : "");
    abort();
}

static inline _Noreturn void st_fatal_code(const char *component, const char *detail, uint64_t code)
{
    fprintf(stderr, "smalltalk runtime: fatal: %s%s%s (code %llu)\n", component ? component : "runtime",
            detail && detail[0] ? ": " : "", detail ? detail : "", (unsigned long long)code);
    abort();
}

#endif
