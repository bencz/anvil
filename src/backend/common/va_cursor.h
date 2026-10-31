/* va_arg over a va_list that is a plain cursor into argument slots (i386,
 * Win64, Darwin ARM64, the 64-bit PowerPC ELF ABIs): the expansion every
 * such ABI shares, parameterized by its slot rules. */
#ifndef ANVIL_BACKEND_VA_CURSOR_H
#define ANVIL_BACKEND_VA_CURSOR_H

#include "anvil/anvil_internal.h"

typedef struct {
    size_t slot;            /* bytes per argument slot */
    bool right_justify;     /* values narrower than a slot sit at its end (big-endian) */
    /* Aggregates of these sizes travel by value in slots; others as a
     * pointer to a copy (one slot). NULL: every aggregate by value. */
    bool (*aggregate_by_value)(size_t size);
} anvil_va_cursor_rules_t;

/* Address of the next argument of `type` (a scalar or a sized aggregate)
 * through the cursor stored at `storage` (an i8**), which advances. */
anvil_value_t *anvil_va_cursor_arg(anvil_ctx_t *ctx, const anvil_va_cursor_rules_t *rules, anvil_value_t *storage, anvil_type_t *type,
                                   const char *abi_name, const char *name);

/* va_copy of a cursor va_list (the cursor is the whole state). */
anvil_value_t *anvil_va_cursor_copy(anvil_ctx_t *ctx, anvil_value_t *cursor, const char *abi_name, const char *name);
bool anvil_va_cursor_copy_into(anvil_ctx_t *ctx, anvil_value_t *destination, anvil_value_t *cursor, const char *abi_name);

/* A scalar integer, pointer or floating-point type, or a sized aggregate. */
bool anvil_va_arg_type_ok(const anvil_type_t *type);

#endif
