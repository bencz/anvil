#include "arm64_internal.h"

static anvil_type_t *aapcs64_cursor_type(anvil_ctx_t *ctx)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *integer = anvil_type_i32(ctx);
    anvil_type_t *fields[] = { pointer, pointer, pointer, integer, integer };
    return anvil_type_struct(ctx, NULL, fields, 5);
}

anvil_value_t *anvil_arm64_build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, byte);
    if (ctx->abi != ANVIL_ABI_SYSV || !anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)) ||
        (!anvil_type_is_integer(type) && !anvil_type_is_floating(type) && type->kind != ANVIL_TYPE_PTR) || type->size > 8)
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_arg requires an i8** cursor and a scalar integer, pointer or floating-point type");
        return NULL;
    }

    anvil_type_t *state = aapcs64_cursor_type(ctx);
    if (!state)
        return NULL;

    anvil_value_t *cursor = anvil_build_load(ctx, pointer, storage, "va.cursor");
    cursor = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, state), "va.state");
    bool floating = anvil_type_is_floating(type);
    anvil_value_t *stack_address = anvil_build_struct_gep(ctx, state, cursor, 0, "va.stack.address");
    anvil_value_t *top_address = anvil_build_struct_gep(ctx, state, cursor, floating ? 2 : 1, "va.top.address");
    anvil_value_t *offset_address = anvil_build_struct_gep(ctx, state, cursor, floating ? 4 : 3, "va.offset.address");
    anvil_value_t *stack = anvil_build_load(ctx, pointer, stack_address, "va.stack");
    anvil_value_t *top = anvil_build_load(ctx, pointer, top_address, "va.top");
    anvil_value_t *offset = anvil_build_load(ctx, anvil_type_i32(ctx), offset_address, "va.offset");
    anvil_value_t *available = anvil_build_cmp_lt(ctx, offset, anvil_const_i32(ctx, 0), "va.register.available");
    anvil_value_t *index = anvil_build_sext(ctx, offset, anvil_type_i64(ctx), "va.register.offset");
    anvil_value_t *register_pointer = anvil_build_gep(ctx, byte, top, &index, 1, "va.register");
    anvil_value_t *address = anvil_build_select(ctx, available, register_pointer, stack, "va.argument");
    anvil_value_t *next_offset = anvil_build_add(ctx, offset, anvil_const_i32(ctx, floating ? 16 : 8), "va.offset.next");
    next_offset = anvil_build_select(ctx, available, next_offset, offset, "va.offset.selected");
    anvil_value_t *step = anvil_const_i64(ctx, 8);
    anvil_value_t *next_stack = anvil_build_gep(ctx, byte, stack, &step, 1, "va.stack.next");
    next_stack = anvil_build_select(ctx, available, stack, next_stack, "va.stack.selected");
    if (!anvil_build_store(ctx, next_offset, offset_address) || !anvil_build_store(ctx, next_stack, stack_address))
        return NULL;

    return anvil_build_bitcast(ctx, address, anvil_type_ptr(ctx, type), name);
}

anvil_value_t *anvil_arm64_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (ctx->abi != ANVIL_ABI_SYSV || !anvil_types_equal(cursor->type, pointer))
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_copy requires an i8* cursor");
        return NULL;
    }

    anvil_type_t *state = aapcs64_cursor_type(ctx);
    if (!state)
        return NULL;

    anvil_value_t *copy = anvil_build_alloca(ctx, state, "va.copy.state");
    if (!anvil_arm64_build_va_copy_into(be, copy, cursor))
        return NULL;

    return anvil_build_bitcast(ctx, copy, pointer, name);
}

bool anvil_arm64_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (ctx->abi != ANVIL_ABI_SYSV || !anvil_types_equal(cursor->type, pointer))
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_copy_into requires an i8* cursor");
        return false;
    }

    anvil_type_t *state = aapcs64_cursor_type(ctx);
    if (!state)
        return false;

    anvil_value_t *copy = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, state), "va.copy.destination");
    anvil_value_t *source = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, state), "va.copy.source");
    for (size_t field = 0; field < 5; field++)
    {
        anvil_value_t *from = anvil_build_struct_gep(ctx, state, source, field, "va.copy.from");
        anvil_value_t *to = anvil_build_struct_gep(ctx, state, copy, field, "va.copy.to");
        anvil_value_t *value = anvil_build_load(ctx, state->data.struc.fields[field], from, "va.copy.field");
        if (!value || !anvil_build_store(ctx, value, to))
            return false;
    }

    return true;
}
