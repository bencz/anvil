/*
 * Variadic callees for the PowerPC ABIs.
 *
 * 64-bit ELF (v1 and v2): every argument has a doubleword in the parameter
 * save area the caller provides, so va_list is a plain cursor into it. The
 * callee homes r3-r10 there; values narrower than a doubleword sit at its
 * high-address end on big-endian targets.
 *
 * 32-bit SVR4: va_list points at { u8 gpr, u8 fpr, u16 reserved,
 * void *overflow_arg_area, void *reg_save_area }. The register save area
 * holds r3-r10 (32 bytes) then f1-f8 (64 bytes); arguments past the
 * registers are read from the overflow area, 64-bit ones 8-aligned and
 * 64-bit integers from an even register pair.
 */

#include "../ppc_internal.h"
#include "../../common/va_cursor.h"

static bool ppc_va_scalar(anvil_type_t *type)
{
    return (anvil_type_is_integer(type) || type->kind == ANVIL_TYPE_PTR || type->kind == ANVIL_TYPE_F64) && type->size <= 8;
}

/* ------------------------------------------------------------------
 * 64-bit ELF: a cursor into the parameter save area.
 * ------------------------------------------------------------------ */

bool ppc_elf64_add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const ppc_arg_state_t *named)
{
    return anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_VA_START, def, named->stack);
}

void ppc_elf64_emit_va_register_save(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start)
{
    for (int64_t word = va_start->imm / 8; word < 8; word++)
        ppc_emit_mem_access(emit, "std", ppc_gpr_names[3 + word], "%r31", emit->desc->incoming_arg_offset + word * 8);
}

void ppc_elf64_emit_va_start(ppc_mir_emit_t *emit, int dst, const anvil_mir_instr_info_t *va_start)
{
    ppc_emit_addi_large(emit, dst, 31, emit->desc->incoming_arg_offset + va_start->imm);
}

/* Doublewords of the parameter save area; on big-endian, anything
 * narrower than a doubleword (aggregates included) sits at its end. */
anvil_value_t *ppc_elf64_build_va_arg(anvil_backend_t *be, const anvil_ppc_target_desc_t *desc, anvil_value_t *storage, anvil_type_t *type,
                                      const char *name)
{
    anvil_va_cursor_rules_t rules = {8, !desc->little_endian, NULL};
    return anvil_va_cursor_arg(be->ctx, &rules, storage, type, "PowerPC", name);
}

anvil_value_t *ppc_cursor_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    return anvil_va_cursor_copy(be->ctx, cursor, "PowerPC", name);
}

bool ppc_cursor_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    return anvil_va_cursor_copy_into(be->ctx, destination, cursor, "PowerPC");
}

/* ------------------------------------------------------------------
 * 32-bit SVR4: register counters and two areas.
 * ------------------------------------------------------------------ */

enum { ELF32_STATE_BYTES = 16, ELF32_GPR_SAVE = 32, ELF32_FPR_SAVE = 64 };

static anvil_type_t *elf32_state_type(anvil_ctx_t *ctx)
{
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *fields[] = {anvil_type_u8(ctx), anvil_type_u8(ctx), anvil_type_u16(ctx), pointer, pointer};
    return anvil_type_struct(ctx, NULL, fields, 5);
}

bool ppc_elf32_add_va_start(anvil_mir_func_t *mir, anvil_mir_vreg_t def, const ppc_arg_state_t *named)
{
    int slot = anvil_mir_add_frame_slot(mir, ELF32_STATE_BYTES + ELF32_GPR_SAVE + ELF32_FPR_SAVE, 8);
    return slot >= 0 && named->stack >= 0 &&
           anvil_mir_add_va_start(mir, def, slot, (unsigned)named->gpr, (unsigned)named->fpr, (size_t)named->stack);
}

static int64_t elf32_state_offset(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start)
{
    if (va_start->frame_slot < 0 || (size_t)va_start->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return 0;
    }
    return -(int64_t)emit->frame_slot_offsets[va_start->frame_slot];
}

/* All eight GPRs and FPRs: storing an unused FPR is harmless, so the CR6
 * hint the caller sets is not needed. */
void ppc_elf32_emit_va_register_save(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *va_start)
{
    int64_t save = elf32_state_offset(emit, va_start) + ELF32_STATE_BYTES;
    for (int reg = 0; reg < 8 && !emit->failed; reg++) {
        ppc_emit_mem_access(emit, "stw", ppc_gpr_names[3 + reg], "%r31", save + reg * 4);
        ppc_emit_mem_access(emit, "stfd", ppc_fpr_names[1 + reg], "%r31", save + ELF32_GPR_SAVE + reg * 8);
    }
}

/* Fills the state through `dst`, then points `dst` at it. */
void ppc_elf32_emit_va_start(ppc_mir_emit_t *emit, int dst, const anvil_mir_instr_info_t *va_start)
{
    int64_t state = elf32_state_offset(emit, va_start);
    const char *reg = ppc_gpr_names[dst];
    if (emit->failed || va_start->named_gpr > 8 || va_start->named_fpr > 8 || va_start->named_stack_bytes > INT32_MAX) {
        emit->failed = true;
        return;
    }
    anvil_strbuf_appendf(&emit->code, "\tli %s, %u\n", reg, va_start->named_gpr);
    ppc_emit_mem_access(emit, "stb", reg, "%r31", state);
    anvil_strbuf_appendf(&emit->code, "\tli %s, %u\n", reg, va_start->named_fpr);
    ppc_emit_mem_access(emit, "stb", reg, "%r31", state + 1);
    ppc_emit_addi_large(emit, dst, 31, emit->desc->incoming_arg_offset + (int64_t)va_start->named_stack_bytes);
    ppc_emit_mem_access(emit, "stw", reg, "%r31", state + 4);
    ppc_emit_addi_large(emit, dst, 31, state + ELF32_STATE_BYTES);
    ppc_emit_mem_access(emit, "stw", reg, "%r31", state + 8);
    ppc_emit_addi_large(emit, dst, 31, state);
}

static anvil_value_t *elf32_align8(anvil_ctx_t *ctx, anvil_value_t *address)
{
    anvil_type_t *word = anvil_type_u32(ctx);
    anvil_value_t *raw = anvil_build_ptrtoint(ctx, address, word, "va.overflow.raw");
    raw = anvil_build_add(ctx, raw, anvil_const_u32(ctx, 7), "va.overflow.up");
    raw = anvil_build_and(ctx, raw, anvil_const_u32(ctx, ~UINT32_C(7)), "va.overflow.aligned");
    return anvil_build_inttoptr(ctx, raw, address->type, "va.overflow.pointer");
}

static anvil_value_t *elf32_scalar_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name);

/* Aggregates arrive by reference: their pointer is an ordinary word. */
anvil_value_t *ppc_elf32_build_va_arg(anvil_backend_t *be, const anvil_ppc_target_desc_t *desc, anvil_value_t *storage, anvil_type_t *type,
                                      const char *name)
{
    (void)desc;
    anvil_ctx_t *ctx = be->ctx;
    if (type && (type->kind == ANVIL_TYPE_STRUCT || type->kind == ANVIL_TYPE_ARRAY) && type->size) {
        anvil_type_t *word = anvil_type_ptr(ctx, anvil_type_i8(ctx));
        anvil_value_t *slot = elf32_scalar_va_arg(be, storage, word, "va.reference.slot");
        anvil_value_t *object = slot ? anvil_build_load(ctx, word, slot, "va.reference") : NULL;
        return object ? anvil_build_bitcast(ctx, object, anvil_type_ptr(ctx, type), name) : NULL;
    }
    return elf32_scalar_va_arg(be, storage, type, name);
}

/* Branch-free: both candidate addresses are formed and selected. */
static anvil_value_t *elf32_scalar_va_arg(anvil_backend_t *be, anvil_value_t *storage, anvil_type_t *type, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *byte = anvil_type_i8(ctx);
    anvil_type_t *pointer = anvil_type_ptr(ctx, byte);
    anvil_type_t *state_type = elf32_state_type(ctx);
    if (!state_type || !anvil_types_equal(storage->type, anvil_type_ptr(ctx, pointer)) || !ppc_va_scalar(type)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "PowerPC va_arg requires an i8** cursor and a scalar integer, pointer or double");
        return NULL;
    }

    bool floating = type->kind == ANVIL_TYPE_F64;
    bool pair = !floating && type->size == 8;
    anvil_type_t *u8 = anvil_type_u8(ctx);
    anvil_value_t *state = anvil_build_bitcast(ctx, anvil_build_load(ctx, pointer, storage, "va.state.pointer"), anvil_type_ptr(ctx, state_type),
                                               "va.state");
    anvil_value_t *count_address = anvil_build_struct_gep(ctx, state_type, state, floating ? 1 : 0, "va.count.address");
    anvil_value_t *overflow_address = anvil_build_struct_gep(ctx, state_type, state, 3, "va.overflow.address");
    anvil_value_t *save_address = anvil_build_struct_gep(ctx, state_type, state, 4, "va.save.address");
    anvil_value_t *count = anvil_build_load(ctx, u8, count_address, "va.count");
    anvil_value_t *overflow = anvil_build_load(ctx, pointer, overflow_address, "va.overflow");
    anvil_value_t *save = anvil_build_load(ctx, pointer, save_address, "va.save");

    /* A 64-bit integer starts at an even register (r3:r4, r5:r6, ...). */
    if (pair)
        count = anvil_build_add(ctx, count, anvil_build_and(ctx, count, anvil_const_u8(ctx, 1), "va.odd"), "va.count.even");
    unsigned registers = pair ? 2 : 1;
    anvil_value_t *available = anvil_build_cmp_ule(ctx, count, anvil_const_u8(ctx, (uint8_t)(8 - registers)), "va.register.available");

    anvil_value_t *index = anvil_build_zext(ctx, count, anvil_type_u32(ctx), "va.index");
    index = anvil_build_mul(ctx, index, anvil_const_u32(ctx, floating ? 8 : 4), "va.index.bytes");
    if (floating)
        index = anvil_build_add(ctx, index, anvil_const_u32(ctx, ELF32_GPR_SAVE), "va.index.fpr");
    anvil_value_t *register_address = anvil_build_gep(ctx, byte, save, &index, 1, "va.register");

    anvil_value_t *stack_address = type->size == 8 ? elf32_align8(ctx, overflow) : overflow;
    anvil_value_t *stack_step = anvil_const_u32(ctx, type->size == 8 ? 8 : 4);
    anvil_value_t *stack_next = anvil_build_gep(ctx, byte, stack_address, &stack_step, 1, "va.overflow.next");

    anvil_value_t *address = anvil_build_select(ctx, available, register_address, stack_address, "va.argument");
    anvil_value_t *next_count = anvil_build_select(ctx, available, anvil_build_add(ctx, count, anvil_const_u8(ctx, (uint8_t)registers), "va.count.next"),
                                                   anvil_const_u8(ctx, 8), "va.count.selected");
    anvil_value_t *next_overflow = anvil_build_select(ctx, available, overflow, stack_next, "va.overflow.selected");
    if (!anvil_build_store(ctx, next_count, count_address) || !anvil_build_store(ctx, next_overflow, overflow_address))
        return NULL;

    /* Narrow integers sit in the low-order (high-address) end of a word. */
    if (!floating && type->size < 4) {
        anvil_value_t *skip = anvil_const_u32(ctx, (uint32_t)(4 - type->size));
        address = anvil_build_gep(ctx, byte, address, &skip, 1, "va.value");
    }
    return anvil_build_bitcast(ctx, address, anvil_type_ptr(ctx, type), name);
}

anvil_value_t *ppc_elf32_build_va_copy(anvil_backend_t *be, anvil_value_t *cursor, const char *name)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *state_type = elf32_state_type(ctx);
    if (!state_type || !anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "PowerPC va_copy requires an i8* cursor");
        return NULL;
    }
    anvil_value_t *copy = anvil_build_alloca(ctx, state_type, "va.copy.state");
    if (!copy || !ppc_elf32_build_va_copy_into(be, copy, cursor))
        return NULL;
    return anvil_build_bitcast(ctx, copy, pointer, name);
}

bool ppc_elf32_build_va_copy_into(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor)
{
    anvil_ctx_t *ctx = be->ctx;
    anvil_type_t *pointer = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *state_type = elf32_state_type(ctx);
    if (!state_type || !anvil_types_equal(cursor->type, pointer)) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_TYPE, "PowerPC va_copy_into requires an i8* cursor");
        return false;
    }
    anvil_value_t *to_state = anvil_build_bitcast(ctx, destination, anvil_type_ptr(ctx, state_type), "va.copy.destination");
    anvil_value_t *from_state = anvil_build_bitcast(ctx, cursor, anvil_type_ptr(ctx, state_type), "va.copy.source");
    for (size_t field = 0; field < 5; field++) {
        anvil_value_t *from = anvil_build_struct_gep(ctx, state_type, from_state, field, "va.copy.from");
        anvil_value_t *to = anvil_build_struct_gep(ctx, state_type, to_state, field, "va.copy.to");
        anvil_value_t *value = anvil_build_load(ctx, state_type->data.struc.fields[field], from, "va.copy.field");
        if (!value || !anvil_build_store(ctx, value, to))
            return false;
    }
    return true;
}
