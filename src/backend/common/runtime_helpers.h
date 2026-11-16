/* The run-time helpers code calls for what a target's instructions lack:
 * 64-bit integer arithmetic in 32-bit registers and integer/floating
 * conversions.  Their names belong to the ABI's run-time library; each ABI
 * descriptor points at its table. */
#ifndef ANVIL_BACKEND_RUNTIME_HELPERS_H
#define ANVIL_BACKEND_RUNTIME_HELPERS_H

#include <stdbool.h>

typedef struct {
    const char *mul64, *sdiv64, *udiv64, *smod64, *umod64;
    const char *shl64, *lshr64, *ashr64;
    /* Indexed [unsigned][the integer has 64 bits][the floating value is a
     * double]. */
    const char *int_to_fp[2][2][2];
    const char *fp_to_int[2][2][2];
} anvil_runtime_helpers_t;

/* libgcc's (and compiler-rt's) names. */
extern const anvil_runtime_helpers_t anvil_libgcc_helpers;

static inline const char *anvil_runtime_int_to_fp(const anvil_runtime_helpers_t *helpers, bool is_unsigned, bool wide, bool is_double)
{
    return helpers->int_to_fp[is_unsigned][wide][is_double];
}

static inline const char *anvil_runtime_fp_to_int(const anvil_runtime_helpers_t *helpers, bool is_unsigned, bool wide, bool is_double)
{
    return helpers->fp_to_int[is_unsigned][wide][is_double];
}

#endif
