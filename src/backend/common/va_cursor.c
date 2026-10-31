#include "va_cursor.h"

bool anvil_va_arg_type_ok(const anvil_type_t *type)
{
    if (!type || !type->size)
        return false;
    if (type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY)
        return true;
    return (anvil_type_is_integer((anvil_type_t *)type) || anvil_type_is_floating((anvil_type_t *)type) || type->kind == ANVIL_TYPE_PTR) &&
           type->size <= 8;
}

anvil_value_t *anvil_va_cursor_arg(anvil_ctx_t *ctx, const anvil_va_cursor_rules_t *rules, anvil_value_t *storage, anvil_type_t *type,
                                   const char *abi_name, const char *name)
{
    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, byte);
    if (!anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)) || !anvil_va_arg_type_ok(type)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "%s va_arg requires an i8** cursor and a scalar or a sized aggregate", abi_name);
        return NULL;
    }

    bool aggregate = type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY;
    bool by_reference = aggregate && rules->aggregate_by_value && !rules->aggregate_by_value(type->size);
    size_t bytes = by_reference ? rules->slot : (type->size + rules->slot - 1) / rules->slot * rules->slot;
    bool wide_index = anvil_type_size(pointer) == 8;   /* index as wide as an address */
#define VA_OFFSET(n) (wide_index ? anvil_const_i64(ctx, (int64_t)(n)) : anvil_const_i32(ctx, (int32_t)(n)))

    anvil_value_t *cursor = anvil_build_load(ctx, pointer, storage, "va.cursor");
    anvil_value_t *step = VA_OFFSET(bytes);
    anvil_value_t *next = anvil_build_gep(ctx, byte, cursor, &step, 1, "va.cursor.next");
    if (!next || !anvil_build_store(ctx, next, storage))
        return NULL;

    anvil_value_t *address = cursor;
    if (by_reference) {
        anvil_value_t *slot = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, pointer), "va.reference.slot");
        address = anvil_build_load(ctx, pointer, slot, "va.reference");
    } else if (rules->right_justify && type->size < rules->slot) {
        anvil_value_t *skip = VA_OFFSET(rules->slot - type->size);
        address = anvil_build_gep(ctx, byte, cursor, &skip, 1, "va.value");
    }
#undef VA_OFFSET
    return address ? anvil_build_bitcast(ctx, address, anvil_type_ptr(ctx, type), name) : NULL;
}

anvil_value_t *anvil_va_cursor_copy(anvil_ctx_t *ctx, anvil_value_t *cursor, const char *abi_name, const char *name)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "%s va_copy requires an i8* cursor", abi_name);
        return NULL;
    }
    return anvil_build_bitcast(ctx, cursor, pointer, name);
}

bool anvil_va_cursor_copy_into(anvil_ctx_t *ctx, anvil_value_t *destination, anvil_value_t *cursor, const char *abi_name)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "%s va_copy_into requires an i8* cursor", abi_name);
        return false;
    }
    anvil_value_t *slot = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, pointer), "va.copy.destination");
    return slot && anvil_build_store(ctx, cursor, slot);
}
