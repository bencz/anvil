/*
 * ANVIL - ARM64 Linux ABI: AAPCS64 with ELF.
 *
 * Stack arguments take doubleword slots, 16-byte aligned composites start
 * at an even x register, unnamed arguments follow the named rules, and
 * va_list is the AAPCS64 structure over the register save areas.
 */

#include "../arm64_internal.h"
#include "../../common/obj_format.h"

#include <stdio.h>

/* ------------------------------------------------------------------
 * Argument passing.
 * ------------------------------------------------------------------ */

/* Scalars: the next x or v register (C.6, C.9), else a doubleword stack
 * slot (C.10, C.15). */
static arm64_arg_loc_t assign_scalar(const arm64_abi_desc_t *desc, arm64_arg_state_t *state,
                                     bool fp, unsigned size, bool unnamed)
{
    (void)size;
    (void)unnamed;
    arm64_arg_loc_t loc = {-1, 0};
    loc.reg = fp ? arm64_aapcs64_take_regs(&state->fpr, desc->num_fpr_args, 1)
                 : arm64_aapcs64_take_regs(&state->gpr, desc->num_gpr_args, 1);
    if (loc.reg < 0) {
        loc.offset = state->stack;
        state->stack += 8;
    }
    return loc;
}

/* Aggregates: all parts in consecutive registers, 16-byte aligned
 * composites from an even x register (C.8), else the memory image.
 * Unnamed arguments are assigned like named ones. */
static arm64_arg_loc_t assign_aggregate(const arm64_abi_desc_t *desc, arm64_arg_state_t *state,
                                        const arm64_agg_plan_t *plan, bool unnamed)
{
    (void)unnamed;
    arm64_arg_loc_t loc = {-1, 0};
    if (plan->fp) {
        loc.reg = arm64_aapcs64_take_regs(&state->fpr, desc->num_fpr_args, plan->num_parts);
    } else {
        if (plan->align >= 16)
            state->gpr = (state->gpr + 1) & ~(size_t)1;
        loc.reg = arm64_aapcs64_take_regs(&state->gpr, desc->num_gpr_args, plan->num_parts);
    }
    if (loc.reg < 0)
        loc.offset = arm64_aapcs64_aggregate_stack(state, plan);
    return loc;
}

/* ------------------------------------------------------------------
 * Variadic functions.
 *
 * va_start builds, in a 224-byte frame object, the cursor
 *   { stack, gr_top, vr_top, gr_offs, vr_offs }   (32 bytes)
 * followed by the saved x0-x7 (64 bytes) and q0-q7 (128 bytes), which the
 * prologue stores.
 * ------------------------------------------------------------------ */

static bool add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const arm64_arg_state_t *named)
{
    int slot = anvil_mir_add_frame_slot(mir, 224, 16);
    return slot >= 0 &&
           anvil_mir_add_va_start(mir, def, slot, (unsigned)named->gpr, (unsigned)named->fpr, (size_t)named->stack);
}

static void emit_va_register_save(anvil_strbuf_t *code, int frame_offset)
{
    for (unsigned reg = 0; reg < 8; reg++) {
        arm64_emit_frame_access(code, "str", arm64_xreg_names[reg], frame_offset - 32 - (int)reg * 8);
        char vector[8];
        snprintf(vector, sizeof(vector), "q%u", reg);
        arm64_emit_frame_access(code, "str", vector, frame_offset - 96 - (int)reg * 16);
    }
}

static void emit_va_start(anvil_strbuf_t *code, const char *dst, int frame_offset, const anvil_mir_instr_info_t *instr)
{
    arm64_emit_mov_imm(code, "x16", frame_offset);
    anvil_strbuf_append(code, "\tsub x17, x29, x16\n");
    arm64_emit_mov_imm(code, "x16", (int64_t)instr->named_stack_bytes + 16);
    anvil_strbuf_append(code, "\tadd x16, x29, x16\n\tstr x16, [x17]\n");
    anvil_strbuf_append(code, "\tadd x16, x17, #96\n\tstr x16, [x17, #8]\n");
    anvil_strbuf_append(code, "\tadd x16, x17, #224\n\tstr x16, [x17, #16]\n");
    arm64_emit_mov_imm(code, "w16", -(int64_t)(8 - instr->named_gpr) * 8);
    anvil_strbuf_append(code, "\tstr w16, [x17, #24]\n");
    arm64_emit_mov_imm(code, "w16", -(int64_t)(8 - instr->named_fpr) * 16);
    anvil_strbuf_append(code, "\tstr w16, [x17, #28]\n");
    anvil_strbuf_appendf(code, "\tmov %s, x17\n", dst);
}

static anvil_type_t *cursor_type(anvil_ctx_t *ctx)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *integer = anvil_type_i32(ctx);
    anvil_type_t *fields[] = { pointer, pointer, pointer, integer, integer };
    return anvil_type_struct(ctx, NULL, fields, 5);
}

static anvil_value_t *build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);

/* Composites (AAPCS64 B.4): over 16 bytes by reference; an HFA from one
 * 16-byte vector slot per member (copied out); others from consecutive x
 * slots (an even register for 16-byte alignment). One that does not fit
 * the remaining registers is on the stack, as is everything after it. */
static anvil_value_t *aggregate_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx), *pointer = anvil_type_ptr(ctx, byte), *i32 = anvil_type_i32(ctx), *i64 = anvil_type_i64(ctx);
    arm64_agg_plan_t plan;
    anvil_type_t *state = cursor_type(ctx);
    if (!state || !arm64_aapcs64_classify_aggregate(type, false, &plan))
        return NULL;
    if (plan.placement == ARM64_AGG_REFERENCE) {
        anvil_value_t *slot = build_va_arg(be, storage, pointer, "va.reference.slot");
        anvil_value_t *object = slot ? anvil_build_load(ctx, pointer, slot, "va.reference") : NULL;
        return object ? anvil_build_bitcast(ctx, object, anvil_type_ptr(ctx, type), name) : NULL;
    }

    anvil_value_t *cursor = anvil_build_bitcast(ctx, anvil_build_load(ctx, pointer, storage, "va.cursor"), anvil_type_ptr(ctx, state), "va.state");
    anvil_value_t *stack_address = anvil_build_struct_gep(ctx, state, cursor, 0, "va.stack.address");
    anvil_value_t *top = anvil_build_load(ctx, pointer, anvil_build_struct_gep(ctx, state, cursor, plan.fp ? 2 : 1, "va.top.address"), "va.top");
    anvil_value_t *offset_address = anvil_build_struct_gep(ctx, state, cursor, plan.fp ? 4 : 3, "va.offset.address");
    anvil_value_t *stack = anvil_build_load(ctx, pointer, stack_address, "va.stack");
    anvil_value_t *offset = anvil_build_load(ctx, i32, offset_address, "va.offset");
    if (!plan.fp && plan.align >= 16)
        offset = anvil_build_and(ctx, anvil_build_add(ctx, offset, anvil_const_i32(ctx, 15), "va.offset.up"), anvil_const_i32(ctx, -16), "va.offset.even");
    if (plan.align >= 16) {
        anvil_value_t *raw = anvil_build_ptrtoint(ctx, stack, i64, "va.stack.raw");
        raw = anvil_build_and(ctx, anvil_build_add(ctx, raw, anvil_const_i64(ctx, 15), "va.stack.up"), anvil_const_i64(ctx, -16), "va.stack.aligned");
        stack = anvil_build_inttoptr(ctx, raw, pointer, "va.stack.object");
    }
    int32_t slot_bytes = plan.fp ? 16 : 8;
    int32_t used = slot_bytes * (int32_t)plan.num_parts;
    anvil_value_t *next_offset = anvil_build_add(ctx, offset, anvil_const_i32(ctx, used), "va.offset.next");
    anvil_value_t *available = anvil_build_cmp_le(ctx, next_offset, anvil_const_i32(ctx, 0), "va.registers.available");
    anvil_value_t *index = anvil_build_sext(ctx, offset, i64, "va.register.offset");
    anvil_value_t *registers = anvil_build_gep(ctx, byte, top, &index, 1, "va.registers");
    anvil_value_t *step = anvil_const_i64(ctx, (int64_t)((plan.size + 7) & ~(size_t)7));
    anvil_value_t *stack_next = anvil_build_gep(ctx, byte, stack, &step, 1, "va.stack.next");

    anvil_value_t *address;
    if (plan.fp) {
        /* Members are 16 bytes apart in the save area, packed on the stack. */
        unsigned member = plan.parts[0].size;
        anvil_type_t *bits = member == 4 ? i32 : i64;
        anvil_value_t *copy = anvil_build_alloca(ctx, anvil_type_array(ctx, bits, plan.num_parts), "va.aggregate");
        anvil_value_t *slots = anvil_build_bitcast(ctx, copy, anvil_type_ptr(ctx, bits), "va.members");
        for (unsigned part = 0; part < plan.num_parts; part++) {
            anvil_value_t *in_registers = anvil_const_i64(ctx, 16 * (int64_t)part), *in_stack = anvil_const_i64(ctx, (int64_t)(member * part));
            anvil_value_t *source = anvil_build_select(ctx, available, anvil_build_gep(ctx, byte, registers, &in_registers, 1, "va.member.register"),
                                                       anvil_build_gep(ctx, byte, stack, &in_stack, 1, "va.member.stack"), "va.member");
            anvil_value_t *value = anvil_build_load(ctx, bits, anvil_build_bitcast(ctx, source, anvil_type_ptr(ctx, bits), "va.member.pointer"), "va.member.bits");
            anvil_value_t *slot_index = anvil_const_i64(ctx, part);
            if (!value || !anvil_build_store(ctx, value, anvil_build_gep(ctx, bits, slots, &slot_index, 1, "va.member.slot")))
                return NULL;
        }
        address = copy;
    } else {
        address = anvil_build_select(ctx, available, registers, stack, "va.argument");
    }

    /* Out of registers: the offset becomes 0 (later arguments are on the stack too). */
    if (!anvil_build_store(ctx, anvil_build_select(ctx, available, next_offset, anvil_const_i32(ctx, 0), "va.offset.selected"), offset_address) ||
        !anvil_build_store(ctx, anvil_build_select(ctx, available, stack, stack_next, "va.stack.selected"), stack_address))
        return NULL;
    return anvil_build_bitcast(ctx, address, anvil_type_ptr(ctx, type), name);
}

static anvil_value_t *build_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, byte);
    if ((type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY) && type->size && anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)))
        return aggregate_va_arg(be, storage, type, name);
    if (!anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)) ||
        (!anvil_type_is_integer(type) && !anvil_type_is_floating(type) && type->kind != ANVIL_TYPE_PTR) || type->size > 8)
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_arg requires an i8** cursor and a scalar integer, pointer or floating-point type");
        return NULL;
    }

    anvil_type_t *state = cursor_type(ctx);
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

static bool build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer))
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_copy_into requires an i8* cursor");
        return false;
    }

    anvil_type_t *state = cursor_type(ctx);
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

static anvil_value_t *build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    if (!anvil_types_equal(cursor->type, pointer))
    {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "AAPCS64 va_copy requires an i8* cursor");
        return NULL;
    }

    anvil_type_t *state = cursor_type(ctx);
    if (!state)
        return NULL;

    anvil_value_t *copy = anvil_build_alloca(ctx, state, "va.copy.state");
    if (!build_va_copy_into(be, copy, cursor))
        return NULL;

    return anvil_build_bitcast(ctx, copy, pointer, name);
}

/* ------------------------------------------------------------------
 * ELF spelling.
 * ------------------------------------------------------------------ */

static void emit_function_header(anvil_strbuf_t *code, const char *symbol, anvil_linkage_t linkage)
{
    anvil_strbuf_append(code, "\t.text\n");
    anvil_obj_emit_binding(code, &anvil_obj_format_elf, linkage, symbol);
    anvil_strbuf_appendf(code, "\t.type %s, %%function\n", symbol);
    anvil_strbuf_appendf(code, "%s:\n", symbol);
}

static void emit_function_footer(anvil_strbuf_t *code, const char *symbol)
{
    anvil_strbuf_appendf(code, "\t.size %s, .-%s\n", symbol, symbol);
}

static void emit_symbol_address(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend)
{
    if (addend) {
        anvil_strbuf_appendf(code, "\tadrp %s, %s%+lld\n", dst, symbol, (long long)addend);
        anvil_strbuf_appendf(code, "\tadd %s, %s, :lo12:%s%+lld\n", dst, dst, symbol, (long long)addend);
    } else {
        anvil_strbuf_appendf(code, "\tadrp %s, %s\n", dst, symbol);
        anvil_strbuf_appendf(code, "\tadd %s, %s, :lo12:%s\n", dst, dst, symbol);
    }
}

static void emit_symbol_got(anvil_strbuf_t *code, const char *dst, const char *symbol, int64_t addend)
{
    anvil_strbuf_appendf(code, "\tadrp %s, :got:%s\n\tldr %s, [%s, :got_lo12:%s]\n", dst, symbol, dst, dst, symbol);
    if (addend)
        arm64_emit_add_imm(code, dst, addend);
}

const arm64_abi_desc_t arm64_linux_abi = {
    .abi = ANVIL_ABI_SYSV,
    .name = "aapcs64-linux",
    .num_gpr_args = 8,
    .num_fpr_args = 8,
    .indirect_result_reg = ARM64_X8,
    .indirect_call_reg = ARM64_X16,
    .caller_extends_small_ints = false,
    .classify_aggregate = arm64_aapcs64_classify_aggregate,
    .assign_scalar = assign_scalar,
    .assign_aggregate = assign_aggregate,
    .add_va_start = add_va_start,
    .emit_va_register_save = emit_va_register_save,
    .emit_va_start = emit_va_start,
    .build_va_arg = build_va_arg,
    .build_va_copy = build_va_copy,
    .build_va_copy_into = build_va_copy_into,
    .symbol_prefix = "",
    .module_header = "// Generated by ANVIL for ARM64 (AArch64) - Linux\n\t.arch armv8-a\n\t.text\n\n",
    .cstring_section = "\t.section .rodata",
    .emit_function_header = emit_function_header,
    .emit_function_footer = emit_function_footer,
    .emit_symbol_address = emit_symbol_address,
    .emit_symbol_got = emit_symbol_got,
    .reloc_models = (1u << ANVIL_RELOC_STATIC) | (1u << ANVIL_RELOC_PIE) | (1u << ANVIL_RELOC_PIC),
    .default_reloc = ANVIL_RELOC_STATIC,
    .format = &anvil_obj_format_elf,
};
