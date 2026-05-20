/*
 * ANVIL - ARM64 Backend Internal Definitions
 * 
 * Internal structures and helpers for the ARM64 code generator.
 */

#ifndef ARM64_INTERNAL_H
#define ARM64_INTERNAL_H

#include "anvil/anvil_internal.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Register Definitions
 * ============================================================================ */

/* ARM64 general-purpose registers (64-bit) */
#define ARM64_X0   0
#define ARM64_X1   1
#define ARM64_X2   2
#define ARM64_X3   3
#define ARM64_X4   4
#define ARM64_X5   5
#define ARM64_X6   6
#define ARM64_X7   7
#define ARM64_X8   8   /* Indirect result location register */
#define ARM64_X9   9   /* Temporary */
#define ARM64_X10  10  /* Temporary */
#define ARM64_X11  11  /* Temporary */
#define ARM64_X12  12  /* Temporary */
#define ARM64_X13  13  /* Temporary */
#define ARM64_X14  14  /* Temporary */
#define ARM64_X15  15  /* Temporary */
#define ARM64_X16  16  /* IP0 - Intra-procedure scratch */
#define ARM64_X17  17  /* IP1 - Intra-procedure scratch */
#define ARM64_X18  18  /* Platform register (reserved) */
#define ARM64_X19  19  /* Callee-saved */
#define ARM64_X20  20  /* Callee-saved */
#define ARM64_X21  21  /* Callee-saved */
#define ARM64_X22  22  /* Callee-saved */
#define ARM64_X23  23  /* Callee-saved */
#define ARM64_X24  24  /* Callee-saved */
#define ARM64_X25  25  /* Callee-saved */
#define ARM64_X26  26  /* Callee-saved */
#define ARM64_X27  27  /* Callee-saved */
#define ARM64_X28  28  /* Callee-saved */
#define ARM64_FP   29  /* Frame pointer (x29) */
#define ARM64_LR   30  /* Link register (x30) */
#define ARM64_SP   31  /* Stack pointer */
#define ARM64_XZR  32  /* Zero register (special encoding) */

#define ARM64_NUM_GPR       32
#define ARM64_NUM_FPR       32
#define ARM64_NUM_ARG_REGS  8

/* Register classes */
#define ARM64_REG_CLASS_NONE     0
#define ARM64_REG_CLASS_GPR      1
#define ARM64_REG_CLASS_FPR      2

/* ============================================================================
 * String Table Entry
 * ============================================================================ */

typedef struct {
    const char *str;
    char label[32];
    size_t len;
} arm64_string_entry_t;

/* ============================================================================
 * Backend State
 * ============================================================================ */

typedef struct {
    /* Output buffers */
    anvil_strbuf_t code;
    anvil_strbuf_t data;
    
    int string_counter;
    
    /* Context reference for ABI detection */
    anvil_ctx_t *ctx;
    
    /* String table */
    arm64_string_entry_t *strings;
    size_t num_strings;
    size_t strings_cap;
    
} arm64_backend_t;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* ABI detection */
static inline bool arm64_is_darwin(arm64_backend_t *be) {
    return be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
}

static inline const char *arm64_symbol_prefix(arm64_backend_t *be) {
    return arm64_is_darwin(be) ? "_" : "";
}

/* Register names */
extern const char *arm64_xreg_names[33];  /* x0-x30, sp, xzr */
extern const char *arm64_wreg_names[33];  /* w0-w30, wsp, wzr */
extern const char *arm64_dreg_names[32];  /* d0-d31 */
extern const char *arm64_sreg_names[32];  /* s0-s31 */

/* Type size calculation */
int arm64_type_size(anvil_type_t *type);
int arm64_type_align(anvil_type_t *type);
bool arm64_type_is_float(anvil_type_t *type);

#endif /* ARM64_INTERNAL_H */
