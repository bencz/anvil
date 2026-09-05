#include "../x86_64_internal.h"

static anvil_type_t *sysv_cursor_type(anvil_ctx_t *ctx)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *fields[] = {anvil_type_u32(ctx), anvil_type_u32(ctx), pointer, pointer};
    return anvil_type_struct(ctx, NULL, fields, 4);
}

static anvil_value_t *sysv_va_arg(anvil_ctx_t *ctx, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    if ((!anvil_type_is_integer(type) && !anvil_type_is_floating(type) && type->kind != ANVIL_TYPE_PTR) || type->size > 8) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "SysV va_arg currently requires a scalar integer, pointer or floating-point type");
        return NULL;
    }

    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, byte);
    anvil_type_t *cursor_type = sysv_cursor_type(ctx);
    if (!cursor_type)
        return NULL;

    anvil_value_t *cursor = anvil_build_load(ctx, pointer, storage, "va.cursor");
    cursor = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, cursor_type), "va.state");
    bool floating = anvil_type_is_floating(type);
    anvil_value_t *offset_address = anvil_build_struct_gep(ctx, cursor_type, cursor, floating ? 1 : 0, "va.offset.address");
    anvil_value_t *stack_address = anvil_build_struct_gep(ctx, cursor_type, cursor, 2, "va.stack.address");
    anvil_value_t *save_address = anvil_build_struct_gep(ctx, cursor_type, cursor, 3, "va.save.address");
    anvil_value_t *offset = anvil_build_load(ctx, anvil_type_u32(ctx), offset_address, "va.offset");
    anvil_value_t *stack = anvil_build_load(ctx, pointer, stack_address, "va.stack");
    anvil_value_t *save = anvil_build_load(ctx, pointer, save_address, "va.save");
    anvil_value_t *available = anvil_build_cmp_ult(ctx, offset, anvil_const_u32(ctx, floating ? 176 : 48), "va.register.available");
    anvil_value_t *index = anvil_build_zext(ctx, offset, anvil_type_u64(ctx), "va.register.offset");
    anvil_value_t *register_pointer = anvil_build_gep(ctx, byte, save, &index, 1, "va.register");
    anvil_value_t *address = anvil_build_select(ctx, available, register_pointer, stack, "va.argument");
    anvil_value_t *next_offset = anvil_build_add(ctx, offset, anvil_const_u32(ctx, floating ? 16 : 8), "va.offset.next");
    next_offset = anvil_build_select(ctx, available, next_offset, offset, "va.offset.selected");
    anvil_value_t *step = anvil_const_u64(ctx, 8);
    anvil_value_t *next_stack = anvil_build_gep(ctx, byte, stack, &step, 1, "va.stack.next");
    next_stack = anvil_build_select(ctx, available, stack, next_stack, "va.stack.selected");
    if (!anvil_build_store(ctx, next_offset, offset_address) || !anvil_build_store(ctx, next_stack, stack_address))
        return NULL;

    return anvil_build_bitcast(ctx, address, anvil_type_ptr(ctx, type), name);
}

anvil_value_t *anvil_x64_build_va_arg(anvil_backend_t *be, anvil_value_t *cursor_storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *cursor_type = anvil_type_ptr(ctx, byte);
    if (!anvil_types_equal(cursor_storage->type, anvil_type_ptr(ctx, cursor_type))) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "x86-64 va_arg requires i8** cursor storage");
        return NULL;
    }

    if (ctx->abi != ANVIL_ABI_WIN64)
        return sysv_va_arg(ctx, cursor_storage, type, name);

    anvil_abi_value_plan_t plan;
    if (anvil_abi_classify_value(ctx, type, false, &plan) != ANVIL_OK)
        return NULL;

    anvil_value_t *cursor = anvil_build_load(ctx, cursor_type, cursor_storage, "va.cursor");
    anvil_value_t *step = anvil_const_i64(ctx, 8);
    anvil_value_t *next = anvil_build_gep(ctx, byte, cursor, &step, 1, "va.next");
    if (!anvil_build_store(ctx, next, cursor_storage))
        return NULL;

    anvil_type_t *pointer = anvil_type_ptr(ctx, type);
    if (plan.kind == ANVIL_ABI_VALUE_INDIRECT) {
        anvil_value_t *slot = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, pointer), "va.indirect");
        return anvil_build_load(ctx, pointer, slot, name);
    }

    return anvil_build_bitcast(ctx, cursor, pointer, name);
}

anvil_value_t *anvil_x64_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "x86-64 va_copy requires an i8* cursor");
        return NULL;
    }
    if (ctx->abi == ANVIL_ABI_WIN64)
        return cursor;

    anvil_type_t *state = sysv_cursor_type(ctx);
    if (!state)
        return NULL;

    anvil_value_t *copy = anvil_build_alloca(ctx, state, "va.copy.state");
    if (!anvil_x64_build_va_copy_into(be, copy, cursor))
        return NULL;

    return anvil_build_bitcast(ctx, copy, pointer, name);
}

bool anvil_x64_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "x86-64 va_copy_into requires an i8* cursor");
        return false;
    }
    if (ctx->abi == ANVIL_ABI_WIN64) {
        destination = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, pointer), "va.copy.destination");
        return anvil_build_store(ctx, cursor, destination);
    }

    anvil_type_t *state = sysv_cursor_type(ctx);
    if (!state)
        return false;

    anvil_value_t *copy = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, state), "va.copy.destination");
    anvil_value_t *source = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, state), "va.copy.source");
    for (size_t field = 0; field < 4; field++) {
        anvil_value_t *from = anvil_build_struct_gep(ctx, state, source, field, "va.copy.from");
        anvil_value_t *to = anvil_build_struct_gep(ctx, state, copy, field, "va.copy.to");
        anvil_value_t *value = anvil_build_load(ctx, state->data.struc.fields[field], from, "va.copy.field");
        if (!value || !anvil_build_store(ctx, value, to))
            return false;
    }

    return true;
}
