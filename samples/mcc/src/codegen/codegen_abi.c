/* Shared caller/callee transport of C aggregate values. */
#include "codegen_internal.h"
#include "target.h"

bool codegen_abi_uses_param_attrs(mcc_codegen_t *cg)
{
    return anvil_ctx_supports_param_attr(cg->anvil_ctx, ANVIL_PARAM_BYVAL) &&
           anvil_ctx_supports_param_attr(cg->anvil_ctx, ANVIL_PARAM_SRET);
}

bool codegen_abi_plan(mcc_codegen_t *cg, mcc_type_t *type, bool is_return, anvil_abi_value_plan_t *plan)
{
    anvil_type_t *object = codegen_type(cg, type);
    if (!object)
        return false;

    /* The target lowers C by-value aggregates itself: records travel as a
     * pointer to an object (BYVAL argument, SRET result) and the target ABI
     * decides registers, stack or reference. */
    if (codegen_type_is_record(type) && codegen_abi_uses_param_attrs(cg))
    {
        plan->kind = ANVIL_ABI_VALUE_INDIRECT;
        plan->transport_type = anvil_type_ptr(cg->anvil_ctx, object);
        plan->temporary_alignment = anvil_type_align(object);
        return plan->transport_type != NULL;
    }

    if (!codegen_type_is_record(type) || mcc_target_model(cg->mcc_ctx->options.arch)->native_aggregate_plans)
    {
        if (anvil_abi_classify_value(cg->anvil_ctx, object, is_return, plan) == ANVIL_OK)
            return true;

        mcc_error(cg->mcc_ctx, "target ABI cannot transport this C value");
        return false;
    }

    /* Preserve the existing private aggregate convention on other targets.
     * Native aggregate interoperability there requires its own target plan. */
    plan->kind = is_return ? ANVIL_ABI_VALUE_DIRECT : ANVIL_ABI_VALUE_INDIRECT;
    plan->transport_type = is_return ? object : anvil_type_ptr(cg->anvil_ctx, object);
    plan->temporary_alignment = anvil_type_align(object);
    return plan->transport_type != NULL;
}

anvil_value_t *codegen_abi_temporary(mcc_codegen_t *cg, mcc_type_t *type, size_t alignment)
{
    anvil_type_t *object = codegen_type(cg, type);
    if (!object)
        return NULL;

    if (alignment <= anvil_type_align(object))
        return codegen_entry_alloca(cg, object, "abi.object");

    size_t size = anvil_type_size(object);
    if (!alignment || (alignment & (alignment - 1)) || size > SIZE_MAX - alignment + 1)
        return NULL;

    /* Keep the complete rounded range inside a fixed allocation. This also
     * works when the object's natural alignment is smaller than the ABI's. */
    anvil_type_t *storage = anvil_type_array(cg->anvil_ctx, anvil_type_u8(cg->anvil_ctx), size + alignment - 1);
    anvil_value_t *pointer = codegen_entry_alloca(cg, storage, "abi.storage");
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    anvil_type_t *integer = arch->ptr_size == 4 ? anvil_type_u32(cg->anvil_ctx) : anvil_type_u64(cg->anvil_ctx);
    anvil_value_t *address = anvil_build_ptrtoint(cg->anvil_ctx, pointer, integer, "abi.address");
    anvil_value_t *padding = codegen_const_int_for_type(cg, integer, (int64_t)(alignment - 1));
    anvil_value_t *mask = codegen_const_int_for_type(cg, integer, -(int64_t)alignment);
    address = anvil_build_add(cg->anvil_ctx, address, padding, "abi.round");
    address = anvil_build_and(cg->anvil_ctx, address, mask, "abi.align");
    return anvil_build_inttoptr(cg->anvil_ctx, address, anvil_type_ptr(cg->anvil_ctx, object), "abi.object");
}

/* Loop counters use the target's pointer width so 32-bit targets never need
 * 64-bit arithmetic for an object walk. */
static anvil_type_t *index_type(mcc_codegen_t *cg)
{
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    return arch && arch->ptr_size == 4 ? anvil_type_u32(cg->anvil_ctx)
                                       : anvil_type_u64(cg->anvil_ctx);
}

typedef bool (*object_unit_fn)(mcc_codegen_t *cg, anvil_type_t *unit,
                               anvil_value_t *index, void *user);

/* Applies `fn` to every `unit`-typed slot of an object of `count` units:
 * unrolled for small objects, otherwise as one counted loop. */
static bool for_each_object_unit(mcc_codegen_t *cg, anvil_type_t *unit,
                                 size_t count, const char *tag,
                                 object_unit_fn fn, void *user)
{
    anvil_type_t *integer = index_type(cg);
    if (count <= 64)
    {
        for (size_t offset = 0; offset < count; offset++)
            if (!fn(cg, unit, codegen_const_int_for_type(cg, integer, (int64_t)offset), user))
                return false;
        return true;
    }

    anvil_block_t *entry = cg->current_block;
    char loop_name[64];
    char done_name[64];
    int identifier = cg->label_counter++;
    snprintf(loop_name, sizeof(loop_name), "object.%s.%d.loop", tag, identifier);
    snprintf(done_name, sizeof(done_name), "object.%s.%d.done", tag, identifier);
    anvil_block_t *loop = anvil_block_create(cg->current_func, loop_name);
    anvil_block_t *done = anvil_block_create(cg->current_func, done_name);
    if (!loop || !done || !anvil_build_br(cg->anvil_ctx, loop))
        return false;

    codegen_set_current_block(cg, loop);
    anvil_value_t *index = anvil_build_phi(cg->anvil_ctx, integer, "object.index");
    if (!index || !fn(cg, unit, index, user))
        return false;

    anvil_value_t *next = anvil_build_add(cg->anvil_ctx, index, codegen_const_int_for_type(cg, integer, 1), "object.next");
    anvil_value_t *more = anvil_build_cmp_ult(cg->anvil_ctx, next, codegen_const_int_for_type(cg, integer, (int64_t)count), "object.more");
    if (!anvil_phi_add_incoming(index, codegen_const_int_for_type(cg, integer, 0), entry) ||
        !anvil_phi_add_incoming(index, next, loop) || !anvil_build_br_cond(cg->anvil_ctx, more, loop, done))
        return false;

    codegen_set_current_block(cg, done);
    return true;
}

static anvil_value_t *unit_pointer(mcc_codegen_t *cg, anvil_value_t *pointer, anvil_type_t *unit, anvil_value_t *index)
{
    pointer = anvil_build_bitcast(cg->anvil_ctx, pointer, anvil_type_ptr(cg->anvil_ctx, unit), "object.units");
    return anvil_build_gep(cg->anvil_ctx, unit, pointer, &index, 1, "object.unit");
}

static anvil_value_t *byte_pointer(mcc_codegen_t *cg, anvil_value_t *pointer, anvil_value_t *offset)
{
    return unit_pointer(cg, pointer, anvil_type_u8(cg->anvil_ctx), offset);
}

typedef struct
{
    anvil_value_t *source;
    anvil_value_t *destination;
    const anvil_memory_access_t *access;
} copy_state_t;

static bool copy_unit(mcc_codegen_t *cg, anvil_type_t *unit, anvil_value_t *index, void *user)
{
    copy_state_t *state = user;
    anvil_value_t *value = anvil_build_load_ex(cg->anvil_ctx, unit, unit_pointer(cg, state->source, unit, index), state->access, "object.copy");
    return value && anvil_build_store_ex(cg->anvil_ctx, value, unit_pointer(cg, state->destination, unit, index), state->access);
}

bool codegen_copy_object(mcc_codegen_t *cg, mcc_type_t *type, anvil_value_t *source, anvil_value_t *destination, const anvil_memory_access_t *access)
{
    if (!source || !destination)
        return false;

    copy_state_t state = { source, destination, access };
    return for_each_object_unit(cg, anvil_type_u8(cg->anvil_ctx), mcc_type_sizeof(type), "copy", copy_unit, &state);
}

static bool zero_unit(mcc_codegen_t *cg, anvil_type_t *unit, anvil_value_t *index, void *user)
{
    anvil_value_t *destination = user;
    return anvil_build_store(cg->anvil_ctx, codegen_const_int_for_type(cg, unit, 0), unit_pointer(cg, destination, unit, index));
}

bool codegen_zero_object(mcc_codegen_t *cg, mcc_type_t *type, anvil_value_t *destination)
{
    if (!destination)
        return false;

    /* The widest unit that divides both the size and the alignment of the
     * object, and that the target handles natively. */
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(cg->anvil_ctx);
    size_t size = mcc_type_sizeof(type);
    size_t align = mcc_type_alignof(type);
    size_t limit = arch && arch->ptr_size == 4 ? 4 : 8;
    size_t width = 1;
    while (width * 2 <= limit && size % (width * 2) == 0 && align % (width * 2) == 0)
        width *= 2;

    anvil_type_t *unit = width == 8 ? anvil_type_u64(cg->anvil_ctx)
        : width == 4 ? anvil_type_u32(cg->anvil_ctx)
        : width == 2 ? anvil_type_u16(cg->anvil_ctx)
        : anvil_type_u8(cg->anvil_ctx);
    return for_each_object_unit(cg, unit, size / width, "zero", zero_unit, destination);
}

anvil_value_t *codegen_abi_pack(mcc_codegen_t *cg, mcc_type_t *type, anvil_value_t *pointer, const anvil_abi_value_plan_t *plan)
{
    if (plan->kind == ANVIL_ABI_VALUE_INDIRECT)
    {
        anvil_value_t *copy = codegen_abi_temporary(cg, type, plan->temporary_alignment);
        return codegen_copy_object(cg, type, pointer, copy, NULL) ? copy : NULL;
    }

    if (plan->kind == ANVIL_ABI_VALUE_DIRECT)
        return anvil_build_load(cg->anvil_ctx, plan->transport_type, pointer, "abi.aggregate");

    /* Byte accesses avoid assuming the C object is aligned like the integer
     * transport (for example, a struct containing only char[8]). */
    size_t size = anvil_type_size(plan->transport_type);
    bool little = anvil_ctx_get_arch_info(cg->anvil_ctx)->endian == ANVIL_ENDIAN_LITTLE;
    anvil_value_t *result = codegen_const_int_for_type(cg, plan->transport_type, 0);
    for (size_t offset = 0; offset < size; offset++)
    {
        anvil_value_t *index = anvil_const_i64(cg->anvil_ctx, (int64_t)offset);
        anvil_value_t *src = byte_pointer(cg, pointer, index);
        anvil_value_t *value = anvil_build_load(cg->anvil_ctx, anvil_type_u8(cg->anvil_ctx), src, "abi.byte");
        if (size > 1)
            value = anvil_build_zext(cg->anvil_ctx, value, plan->transport_type, "abi.extend");

        size_t shift = (little ? offset : size - 1 - offset) * 8;
        if (shift)
            value = anvil_build_shl(cg->anvil_ctx, value, codegen_const_int_for_type(cg, plan->transport_type, (int64_t)shift), "abi.shift");

        result = anvil_build_or(cg->anvil_ctx, result, value, "abi.pack");
    }

    return result;
}

bool codegen_abi_unpack(mcc_codegen_t *cg, mcc_type_t *type, anvil_value_t *value, anvil_value_t *pointer, const anvil_abi_value_plan_t *plan)
{
    if (plan->kind == ANVIL_ABI_VALUE_INDIRECT)
        return codegen_copy_object(cg, type, value, pointer, NULL);

    if (plan->kind == ANVIL_ABI_VALUE_DIRECT)
        return anvil_build_store(cg->anvil_ctx, value, pointer);

    size_t size = anvil_type_size(plan->transport_type);
    bool little = anvil_ctx_get_arch_info(cg->anvil_ctx)->endian == ANVIL_ENDIAN_LITTLE;
    for (size_t offset = 0; offset < size; offset++)
    {
        anvil_value_t *part = value;
        size_t shift = (little ? offset : size - 1 - offset) * 8;
        if (shift)
            part = anvil_build_shr(cg->anvil_ctx, part, codegen_const_int_for_type(cg, plan->transport_type, (int64_t)shift), "abi.shift");

        if (size > 1)
            part = anvil_build_trunc(cg->anvil_ctx, part, anvil_type_u8(cg->anvil_ctx), "abi.byte");

        anvil_value_t *index = anvil_const_i64(cg->anvil_ctx, (int64_t)offset);
        if (!part || !anvil_build_store(cg->anvil_ctx, part, byte_pointer(cg, pointer, index)))
            return false;
    }

    return true;
}
