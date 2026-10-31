/*
 * ANVIL - i386 variadic arguments.
 *
 * Every 32-bit x86 convention passes unnamed arguments on the stack in
 * 4-byte slots (8 for 64-bit values), so va_list is a pointer to the next
 * slot on all platforms.
 */

#include "../x86_32_internal.h"
#include "../../common/va_cursor.h"

static bool cursor_type_ok(anvil_ctx_t *ctx, anvil_value_t *cursor)
{
    return anvil_types_equal(cursor->type, anvil_type_ptr(ctx, anvil_type_i8(ctx)));
}

/* 4-byte slots; aggregates are stack copies of their rounded size. */
anvil_value_t *anvil_x86_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    static const anvil_va_cursor_rules_t rules = {4, false, NULL};
    return anvil_va_cursor_arg(be->ctx, &rules, storage, type, "i386", name);
}

anvil_value_t *anvil_x86_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    if (!cursor_type_ok(ctx, cursor)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "i386 va_copy requires an i8* cursor");
        return NULL;
    }
    /* The cursor is the whole va_list. */
    return anvil_build_bitcast(ctx, cursor, cursor->type, name);
}

bool anvil_x86_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    anvil_ctx_t *ctx = be->ctx;
    if (!cursor_type_ok(ctx, cursor)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "i386 va_copy_into requires an i8* cursor");
        return false;
    }
    anvil_value_t *slot = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, cursor->type), "va.copy.destination");
    return slot && anvil_build_store(ctx, cursor, slot);
}
