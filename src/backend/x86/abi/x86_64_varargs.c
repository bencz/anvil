#include "../x86_64_internal.h"
#include "../../common/va_cursor.h"

static anvil_type_t *sysv_cursor_type(anvil_ctx_t *ctx)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *fields[] = {anvil_type_u32(ctx), anvil_type_u32(ctx), pointer, pointer};
    return anvil_type_struct(ctx, NULL, fields, 4);
}

static bool win64_aggregate_by_value(size_t size)
{
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static anvil_value_t *sysv_offset_pointer(anvil_ctx_t *ctx, anvil_value_t *base, anvil_value_t *offset, uint32_t extra, const char *name)
{
    anvil_value_t *index = anvil_build_zext(ctx, anvil_build_add(ctx, offset, anvil_const_u32(ctx, extra), "va.part.offset"), anvil_type_u64(ctx),
                                            "va.part.index");
    return anvil_build_gep(ctx, anvil_type_i8(ctx), base, &index, 1, name);
}

/* An aggregate: MEMORY class from the overflow area; otherwise each
 * eightbyte from the GP or SSE save area when all of them still fit (all
 * or none), else from the overflow area, copied into a temporary. */
static anvil_value_t *sysv_aggregate_va_arg(anvil_ctx_t *ctx, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    x64_agg_plan_t plan;
    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(ctx->abi);
    anvil_type_t *cursor_type = sysv_cursor_type(ctx);
    if (!desc || !cursor_type || !x64_classify_aggregate(desc, type, false, &plan))
        return NULL;

    anvil_type_t *byte = anvil_type_i8(ctx), *pointer = anvil_type_ptr(ctx, byte), *u32 = anvil_type_u32(ctx), *u64 = anvil_type_u64(ctx);
    anvil_value_t *state = anvil_build_bitcast(ctx, anvil_build_load(ctx, pointer, storage, "va.cursor"), anvil_type_ptr(ctx, cursor_type), "va.state");
    anvil_value_t *gp_address = anvil_build_struct_gep(ctx, cursor_type, state, 0, "va.gp.address");
    anvil_value_t *fp_address = anvil_build_struct_gep(ctx, cursor_type, state, 1, "va.fp.address");
    anvil_value_t *stack_address = anvil_build_struct_gep(ctx, cursor_type, state, 2, "va.stack.address");
    anvil_value_t *save = anvil_build_load(ctx, pointer, anvil_build_struct_gep(ctx, cursor_type, state, 3, "va.save.address"), "va.save");
    anvil_value_t *stack = anvil_build_load(ctx, pointer, stack_address, "va.stack");

    /* Overflow slots: eightbytes, 16-aligned for 16-aligned types. */
    if (plan.align > 8) {
        anvil_value_t *raw = anvil_build_ptrtoint(ctx, stack, u64, "va.stack.raw");
        raw = anvil_build_and(ctx, anvil_build_add(ctx, raw, anvil_const_u64(ctx, 15), "va.stack.up"), anvil_const_u64(ctx, ~UINT64_C(15)), "va.stack.aligned");
        stack = anvil_build_inttoptr(ctx, raw, pointer, "va.stack.object");
    }
    uint64_t rounded = (plan.size + 7) & ~(uint64_t)7;
    anvil_value_t *step = anvil_const_u64(ctx, rounded);
    anvil_value_t *stack_next = anvil_build_gep(ctx, byte, stack, &step, 1, "va.stack.next");
    if (plan.placement != X64_AGG_REGS) {
        if (!anvil_build_store(ctx, stack_next, stack_address))
            return NULL;
        return anvil_build_bitcast(ctx, stack, anvil_type_ptr(ctx, type), name);
    }

    anvil_value_t *gp = anvil_build_load(ctx, u32, gp_address, "va.gp");
    anvil_value_t *fp = anvil_build_load(ctx, u32, fp_address, "va.fp");
    anvil_value_t *available = anvil_build_and(ctx,
        anvil_build_cmp_ule(ctx, gp, anvil_const_u32(ctx, 48 - 8 * plan.num_gpr), "va.gp.fits"),
        anvil_build_cmp_ule(ctx, fp, anvil_const_u32(ctx, 176 - 16 * plan.num_fpr), "va.fp.fits"), "va.registers.available");

    anvil_type_t *image_type = anvil_type_array(ctx, u64, rounded / 8);
    anvil_value_t *image = anvil_build_alloca(ctx, image_type, "va.aggregate");
    unsigned gp_used = 0, fp_used = 0;
    for (unsigned part = 0; part < plan.num_parts; part++) {
        bool sse = plan.parts[part].fp;
        anvil_value_t *from_registers = sse ? sysv_offset_pointer(ctx, save, fp, 16 * fp_used++, "va.part.fp")
                                            : sysv_offset_pointer(ctx, save, gp, 8 * gp_used++, "va.part.gp");
        anvil_value_t *offset = anvil_const_u64(ctx, 8 * part);
        anvil_value_t *from_stack = anvil_build_gep(ctx, byte, stack, &offset, 1, "va.part.stack");
        anvil_value_t *source = anvil_build_select(ctx, available, from_registers, from_stack, "va.part.source");
        anvil_value_t *word = anvil_build_load(ctx, u64, anvil_build_bitcast(ctx, source, anvil_type_ptr(ctx, u64), "va.part.word.pointer"), "va.part.word");
        anvil_value_t *index = anvil_const_u64(ctx, part);
        anvil_value_t *destination = anvil_build_gep(ctx, u64, anvil_build_bitcast(ctx, image, anvil_type_ptr(ctx, u64), "va.image"), &index, 1, "va.part.slot");
        if (!word || !anvil_build_store(ctx, word, destination))
            return NULL;
    }

    anvil_value_t *gp_next = anvil_build_select(ctx, available, anvil_build_add(ctx, gp, anvil_const_u32(ctx, 8 * plan.num_gpr), "va.gp.used"), gp, "va.gp.next");
    anvil_value_t *fp_next = anvil_build_select(ctx, available, anvil_build_add(ctx, fp, anvil_const_u32(ctx, 16 * plan.num_fpr), "va.fp.used"), fp, "va.fp.next");
    anvil_value_t *stack_after = anvil_build_select(ctx, available, stack, stack_next, "va.stack.selected");
    if (!anvil_build_store(ctx, gp_next, gp_address) || !anvil_build_store(ctx, fp_next, fp_address) || !anvil_build_store(ctx, stack_after, stack_address))
        return NULL;
    return anvil_build_bitcast(ctx, image, anvil_type_ptr(ctx, type), name);
}

static anvil_value_t *sysv_va_arg(anvil_ctx_t *ctx, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    if ((type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY) && type->size)
        return sysv_aggregate_va_arg(ctx, storage, type, name);
    if ((!anvil_type_is_integer(type) && !anvil_type_is_floating(type) && type->kind != ANVIL_TYPE_PTR) || type->size > 8) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "SysV va_arg requires a scalar integer, pointer or floating-point type, or a sized aggregate");
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

    /* Win64: 8-byte slots; aggregates other than 1, 2, 4 or 8 bytes (and
     * nothing else) travel as a pointer to a copy. */
    static const anvil_va_cursor_rules_t rules = {8, false, win64_aggregate_by_value};
    return anvil_va_cursor_arg(ctx, &rules, cursor_storage, type, "Win64", name);
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
