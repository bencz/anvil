#include "ppc_internal.h"

static const char *ppc_gpr_names[] = {"r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
                                      "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"};

static const char *ppc_fpr_names[] = {"f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
                                      "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"};

static int align_int(int value, int align)
{
    return (value + align - 1) & ~(align - 1);
}

static int ppc_mir_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 8;
    int size = (int)((size_bits + 7) / 8);
    if (size <= 0)
        return 8;
    if (size > 8)
        return 8;
    return size;
}

static int ppc_mir_slot_size_bytes(uint16_t size_bits)
{
    if (size_bits == 0)
        return 8;
    int size = (int)((size_bits + 7) / 8);
    return size > 0 ? size : 8;
}

static bool ppc_is_64(const ppc_mir_emit_t *emit)
{
    return emit->desc->word_size == 8;
}

static const char *ppc_store_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "std" : "stw";
}

static const char *ppc_load_word_op(const ppc_mir_emit_t *emit)
{
    return ppc_is_64(emit) ? "ld" : "lwz";
}

static const anvil_mir_vreg_info_t *ppc_vreg_info_checked(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    if (!info)
        emit->failed = true;
    return info;
}

static const anvil_regalloc_assignment_t *ppc_assignment_checked(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, vreg);
    if (!assignment || assignment->spilled || assignment->phys_reg < 0 || assignment->phys_reg >= 32) {
        emit->failed = true;
        return NULL;
    }
    return assignment;
}

static const char *ppc_reg_name(ppc_mir_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = ppc_vreg_info_checked(emit, vreg);
    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, vreg);
    if (!info || !assignment)
        return "?";
    return info->reg_class == ANVIL_MIR_REG_FPR ? ppc_fpr_names[assignment->phys_reg] : ppc_gpr_names[assignment->phys_reg];
}

static bool ppc_offset_fits_dform(int64_t offset)
{
    return offset >= -32768 && offset <= 32767;
}

static void ppc_emit_load_imm(ppc_mir_emit_t *emit, int reg, int64_t imm)
{
    if (reg < 0 || reg >= 32) {
        emit->failed = true;
        return;
    }

    const char *r = ppc_gpr_names[reg];
    if (imm >= -32768 && imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tli %s, %lld\n", r, (long long)imm);
        return;
    }

    if (!ppc_is_64(emit) || (imm >= INT32_MIN && imm <= INT32_MAX)) {
        uint32_t raw = (uint32_t)imm;
        uint32_t hi = (raw >> 16) & 0xffffu;
        uint32_t lo = raw & 0xffffu;
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, hi);
        if (lo != 0) {
            anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, lo);
        }
        return;
    }

    uint64_t raw = (uint64_t)imm;
    uint16_t c0 = (uint16_t)((raw >> 48) & 0xffffu);
    uint16_t c1 = (uint16_t)((raw >> 32) & 0xffffu);
    uint16_t c2 = (uint16_t)((raw >> 16) & 0xffffu);
    uint16_t c3 = (uint16_t)(raw & 0xffffu);
    anvil_strbuf_appendf(&emit->code, "\tlis %s, %u\n", r, (unsigned)c0);
    anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, (unsigned)c1);
    anvil_strbuf_appendf(&emit->code, "\tsldi %s, %s, 32\n", r, r);
    if (c2 != 0) {
        anvil_strbuf_appendf(&emit->code, "\toris %s, %s, %u\n", r, r, (unsigned)c2);
    }
    if (c3 != 0) {
        anvil_strbuf_appendf(&emit->code, "\tori %s, %s, %u\n", r, r, (unsigned)c3);
    }
}

static void ppc_emit_addi_large(ppc_mir_emit_t *emit, int dst, int base, int64_t offset)
{
    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %lld\n", ppc_gpr_names[dst], ppc_gpr_names[base], (long long)offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, r11\n", ppc_gpr_names[dst], ppc_gpr_names[base]);
}

static void ppc_emit_local_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    int64_t dform = -(int64_t)offset;
    if (ppc_offset_fits_dform(dform)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %lld(r31)\n", op, reg, (long long)dform);
        return;
    }

    ppc_emit_load_imm(emit, 11, dform);
    anvil_strbuf_append(&emit->code, "\tadd r11, r31, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static void ppc_emit_sp_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %d(r1)\n", op, reg, offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_append(&emit->code, "\tadd r11, r1, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static void ppc_emit_incoming_stack_access(ppc_mir_emit_t *emit, const char *op, const char *reg, int offset)
{
    if (offset < 0) {
        emit->failed = true;
        return;
    }

    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %d(r31)\n", op, reg, offset);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_append(&emit->code, "\tadd r11, r31, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static const char *ppc_load_op(const anvil_ppc_target_desc_t *desc, anvil_mir_reg_class_t reg_class, int size, bool is_signed)
{
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size <= 4 ? "lfs" : "lfd";
    if (is_signed) {
        switch (size) {
        case 1:
            return "lbz";
        case 2:
            return "lha";
        case 4:
            return desc->word_size == 8 ? "lwa" : "lwz";
        default:
            return "ld";
        }
    }
    switch (size) {
    case 1:
        return "lbz";
    case 2:
        return "lhz";
    case 4:
        return "lwz";
    default:
        return "ld";
    }
}

static const char *ppc_store_op(const anvil_ppc_target_desc_t *desc, anvil_mir_reg_class_t reg_class, int size)
{
    (void)desc;
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size <= 4 ? "stfs" : "stfd";
    switch (size) {
    case 1:
        return "stb";
    case 2:
        return "sth";
    case 4:
        return "stw";
    default:
        return "std";
    }
}

static void ppc_emit_sign_extend_loaded_byte(ppc_mir_emit_t *emit, const anvil_mir_vreg_info_t *info, const char *reg, int size)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || !info->is_signed)
        return;
    if (size == 1) {
        anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", reg, reg);
    }
}

static void ppc_emit_base_offset_access(ppc_mir_emit_t *emit, const char *op, const char *reg, const char *base, int64_t offset)
{
    if (ppc_offset_fits_dform(offset)) {
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %lld(%s)\n", op, reg, (long long)offset, base);
        return;
    }

    ppc_emit_load_imm(emit, 11, offset);
    anvil_strbuf_appendf(&emit->code, "\tadd r11, %s, r11\n", base);
    anvil_strbuf_appendf(&emit->code, "\t%s %s, 0(r11)\n", op, reg);
}

static bool ppc_emit_label(ppc_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s:\n", anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool ppc_emit_branch_target(ppc_mir_emit_t *emit, anvil_mir_block_t block)
{
    anvil_mir_block_info_t info;
    if (!anvil_mir_get_block_info(emit->mir, block, &info))
        return false;
    const char *name = info.name && info.name[0] ? info.name : "block";
    anvil_strbuf_appendf(&emit->code, ".L%s_%s", anvil_mir_func_name(emit->mir), name);
    return true;
}

static bool ppc_instr_has_call(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return true;
        switch (info.op) {
        case ANVIL_MIR_OP_CALL:
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
            return true;
        default:
            break;
        }
    }
    return false;
}

static bool ppc_instr_has_dynamic_alloca(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return true;
        if (info.op == ANVIL_MIR_OP_DYN_ALLOCA)
            return true;
    }
    return false;
}

static bool ppc_instr_has_incoming_stack_arg(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info))
            return true;
        if (info.op == ANVIL_MIR_OP_INCOMING_STACK_ARG)
            return true;
    }
    return false;
}

static bool ppc_instr_needs_fp_scratch(const anvil_mir_func_t *mir)
{
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr))
            return true;

        if (instr.def == ANVIL_MIR_NO_VREG) {
            continue;
        }
        const anvil_mir_vreg_info_t *def_info = anvil_mir_get_vreg_info(mir, instr.def);
        if (!def_info)
            return true;

        if (instr.op == ANVIL_MIR_OP_MOV && instr.has_imm && def_info->reg_class == ANVIL_MIR_REG_FPR) {
            return true;
        }

        if ((instr.op == ANVIL_MIR_OP_COPY || instr.op == ANVIL_MIR_OP_BITCAST) && instr.num_uses == 1) {
            anvil_mir_vreg_t src = anvil_mir_get_instr_use(mir, i, 0);
            const anvil_mir_vreg_info_t *src_info = anvil_mir_get_vreg_info(mir, src);
            if (!src_info)
                return true;
            if (src_info->reg_class != def_info->reg_class && (src_info->reg_class == ANVIL_MIR_REG_FPR || def_info->reg_class == ANVIL_MIR_REG_FPR)) {
                return true;
            }
        }
    }
    return false;
}

static bool ppc_scan_outgoing_stack_args(ppc_mir_emit_t *emit)
{
    int outgoing_size = 0;

    for (size_t i = 0; i < anvil_mir_num_instrs(emit->mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(emit->mir, i, &info))
            return false;
        if (info.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;
        if (!info.has_imm || info.imm < 0 || info.num_uses != 1)
            return false;
        if (info.imm > INT32_MAX - 8)
            return false;

        anvil_mir_vreg_t arg = anvil_mir_get_instr_use(emit->mir, i, 0);
        const anvil_mir_vreg_info_t *arg_info = anvil_mir_get_vreg_info(emit->mir, arg);
        if (!arg_info)
            return false;

        int slot_size = align_int(ppc_mir_size_bytes(arg_info->size_bits), (int)emit->desc->word_size);
        if (slot_size < (int)emit->desc->word_size) {
            slot_size = (int)emit->desc->word_size;
        }
        int end = (int)info.imm + slot_size;
        if (end > outgoing_size)
            outgoing_size = end;
    }

    emit->outgoing_size = align_int(outgoing_size, 16);
    return true;
}

static bool ppc_prepare_frame(ppc_mir_emit_t *emit)
{
    for (size_t i = 0; i < 32; i++) {
        emit->gpr_save_offsets[i] = -1;
        emit->fpr_save_offsets[i] = -1;
    }
    emit->fp_const_scratch_offset = -1;

    if (!ppc_scan_outgoing_stack_args(emit))
        return false;

    int word = (int)emit->desc->word_size;
    int offset = word;
    emit->gpr_save_offsets[31] = offset;

    for (size_t i = 0; i < anvil_mir_num_vregs(emit->mir); i++) {
        const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(emit->mir, (anvil_mir_vreg_t)i);
        if (!assignment || assignment->spilled)
            continue;

        if (assignment->reg_class == ANVIL_MIR_REG_GPR && assignment->phys_reg >= 14 && assignment->phys_reg <= 30 && emit->gpr_save_offsets[assignment->phys_reg] < 0) {
            offset += word;
            emit->gpr_save_offsets[assignment->phys_reg] = offset;
        } else if (assignment->reg_class == ANVIL_MIR_REG_FPR && assignment->phys_reg >= 14 && assignment->phys_reg <= 31 && emit->fpr_save_offsets[assignment->phys_reg] < 0) {
            offset = align_int(offset, 8);
            offset += 8;
            emit->fpr_save_offsets[assignment->phys_reg] = offset;
        }
    }

    emit->num_frame_slot_offsets = anvil_mir_num_frame_slots(emit->mir);
    if (emit->num_frame_slot_offsets > 0) {
        emit->frame_slot_offsets = calloc(emit->num_frame_slot_offsets, sizeof(*emit->frame_slot_offsets));
        if (!emit->frame_slot_offsets)
            return false;
    }

    for (size_t i = 0; i < emit->num_frame_slot_offsets; i++) {
        anvil_mir_frame_slot_info_t slot;
        if (!anvil_mir_get_frame_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        int align = slot.align_bytes ? slot.align_bytes : word;
        if (align > 16)
            align = 16;
        offset = align_int(offset, align);
        offset += ppc_mir_slot_size_bytes(slot.size_bits);
        emit->frame_slot_offsets[i] = offset;
    }

    emit->num_spill_offsets = anvil_mir_num_spills(emit->mir);
    if (emit->num_spill_offsets > 0) {
        emit->spill_offsets = calloc(emit->num_spill_offsets, sizeof(*emit->spill_offsets));
        if (!emit->spill_offsets)
            return false;
    }

    for (size_t i = 0; i < emit->num_spill_offsets; i++) {
        anvil_mir_spill_slot_info_t slot;
        if (!anvil_mir_get_spill_slot_info(emit->mir, (int)i, &slot)) {
            return false;
        }
        offset = align_int(offset, ppc_mir_size_bytes(slot.size_bits));
        offset += ppc_mir_size_bytes(slot.size_bits);
        emit->spill_offsets[i] = offset;
    }

    if (ppc_instr_needs_fp_scratch(emit->mir)) {
        offset = align_int(offset, 8);
        offset += 8;
        emit->fp_const_scratch_offset = offset;
    }

    int outgoing_end = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    int needed = offset + outgoing_end;
    if (needed < outgoing_end)
        return false;

    emit->has_frame = needed > word || emit->outgoing_size > 0 || ppc_instr_has_call(emit->mir) || ppc_instr_has_dynamic_alloca(emit->mir) || ppc_instr_has_incoming_stack_arg(emit->mir) ||
                      ppc_instr_needs_fp_scratch(emit->mir);
    if (!emit->has_frame) {
        emit->frame_size = 0;
        return true;
    }

    if (needed < (int)emit->desc->min_frame_size) {
        needed = (int)emit->desc->min_frame_size;
    }
    emit->frame_size = align_int(needed, 16);
    return true;
}

static void ppc_emit_frame_adjust(ppc_mir_emit_t *emit, const char *op, int frame_size)
{
    if (frame_size <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\t%s r1, %s%d(r1)\n", ppc_is_64(emit) ? "stdu" : "stwu", op[0] == '-' ? "-" : "", frame_size);
        return;
    }

    ppc_emit_load_imm(emit, 11, frame_size);
    anvil_strbuf_append(&emit->code, "\tneg r11, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r1, r1, r11\n", ppc_is_64(emit) ? "stdux" : "stwux");
}

static void ppc_emit_prologue(ppc_mir_emit_t *emit)
{
    emit->desc->abi_ops->emit_function_header(emit);
    if (!emit->has_frame)
        return;

    anvil_strbuf_append(&emit->code, "\tmflr r0\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r0, %u(r1)\n", ppc_store_word_op(emit), emit->desc->lr_save_offset);
    if (emit->desc->uses_function_descriptors && emit->desc->toc_save_offset > 0) {
        anvil_strbuf_appendf(&emit->code, "\tstd r2, %u(r1)\n", emit->desc->toc_save_offset);
    }

    ppc_emit_frame_adjust(emit, "-", emit->frame_size);

    int r31_offset = emit->frame_size - emit->gpr_save_offsets[31];
    anvil_strbuf_appendf(&emit->code, "\t%s r31, %d(r1)\n", ppc_store_word_op(emit), r31_offset);
    ppc_emit_addi_large(emit, 31, 1, emit->frame_size);

    for (int reg = 14; reg <= 30; reg++) {
        if (emit->gpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, ppc_store_word_op(emit), ppc_gpr_names[reg], emit->gpr_save_offsets[reg]);
        }
    }
    for (int reg = 14; reg <= 31; reg++) {
        if (emit->fpr_save_offsets[reg] >= 0) {
            ppc_emit_local_access(emit, "stfd", ppc_fpr_names[reg], emit->fpr_save_offsets[reg]);
        }
    }
}

static void ppc_emit_stack_restore(ppc_mir_emit_t *emit)
{
    if (emit->frame_size <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\taddi r1, r1, %d\n", emit->frame_size);
        return;
    }

    ppc_emit_load_imm(emit, 11, emit->frame_size);
    anvil_strbuf_append(&emit->code, "\tadd r1, r1, r11\n");
}

static void ppc_emit_epilogue(ppc_mir_emit_t *emit)
{
    if (emit->has_frame) {
        for (int reg = 14; reg <= 31; reg++) {
            if (emit->fpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, "lfd", ppc_fpr_names[reg], emit->fpr_save_offsets[reg]);
            }
        }
        for (int reg = 30; reg >= 14; reg--) {
            if (emit->gpr_save_offsets[reg] >= 0) {
                ppc_emit_local_access(emit, ppc_load_word_op(emit), ppc_gpr_names[reg], emit->gpr_save_offsets[reg]);
            }
        }

        if (emit->desc->uses_function_descriptors && emit->desc->toc_save_offset > 0) {
            anvil_strbuf_appendf(&emit->code, "\tld r2, %u(r31)\n", emit->desc->toc_save_offset);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s r0, %u(r31)\n", ppc_load_word_op(emit), emit->desc->lr_save_offset);
        anvil_strbuf_append(&emit->code, "\tmtlr r0\n");
        if (ppc_instr_has_dynamic_alloca(emit->mir)) {
            anvil_strbuf_append(&emit->code, "\tmr r1, r31\n");
        } else {
            ppc_emit_stack_restore(emit);
        }
        anvil_strbuf_appendf(&emit->code, "\t%s r31, -%u(r1)\n", ppc_load_word_op(emit), emit->desc->word_size);
    }
    anvil_strbuf_append(&emit->code, "\tblr\n");
}

static bool ppc_get_uses2(const anvil_mir_func_t *mir, size_t instr_index, anvil_mir_vreg_t *lhs, anvil_mir_vreg_t *rhs)
{
    *lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    *rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    return *lhs != ANVIL_MIR_NO_VREG && *rhs != ANVIL_MIR_NO_VREG;
}

static void ppc_emit_copy(ppc_mir_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, dst);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const anvil_regalloc_assignment_t *dst_assignment = ppc_assignment_checked(emit, dst);
    const anvil_regalloc_assignment_t *src_assignment = ppc_assignment_checked(emit, src);
    if (!dst_info || !src_info || !dst_assignment || !src_assignment)
        return;

    const char *dst_reg = ppc_reg_name(emit, dst);
    const char *src_reg = ppc_reg_name(emit, src);
    if (emit->failed)
        return;
    if (dst_assignment->phys_reg == src_assignment->phys_reg && dst_info->reg_class == src_info->reg_class) {
        return;
    }

    if (dst_info->reg_class == ANVIL_MIR_REG_GPR && src_info->reg_class == ANVIL_MIR_REG_GPR) {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst_reg, src_reg);
        return;
    }
    if (dst_info->reg_class == ANVIL_MIR_REG_FPR && src_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n", dst_reg, src_reg);
        return;
    }

    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_GPR && dst_info->reg_class == ANVIL_MIR_REG_FPR) {
        int size = ppc_mir_size_bytes(dst_info->size_bits);
        ppc_emit_local_access(emit, size <= 4 ? "stw" : ppc_store_word_op(emit), src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, size <= 4 ? "lfs" : "lfd", dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    if (src_info->reg_class == ANVIL_MIR_REG_FPR && dst_info->reg_class == ANVIL_MIR_REG_GPR) {
        int size = ppc_mir_size_bytes(src_info->size_bits);
        ppc_emit_local_access(emit, size <= 4 ? "stfs" : "stfd", src_reg, emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, size <= 4 ? "lwz" : ppc_load_word_op(emit), dst_reg, emit->fp_const_scratch_offset);
        return;
    }

    emit->failed = true;
}

static void ppc_emit_binary(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    if (!def_info)
        return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        const char *op = NULL;
        bool single = def_info->size_bits == 32;
        switch (info->op) {
        case ANVIL_MIR_OP_ADD:
            op = single ? "fadds" : "fadd";
            break;
        case ANVIL_MIR_OP_SUB:
            op = single ? "fsubs" : "fsub";
            break;
        case ANVIL_MIR_OP_MUL:
            op = single ? "fmuls" : "fmul";
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            op = single ? "fdivs" : "fdiv";
            break;
        default:
            break;
        }
        if (!op) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", op, dst, a, b);
        return;
    }

    bool wide = ppc_is_64(emit) && def_info->size_bits > 32;
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        anvil_strbuf_appendf(&emit->code, "\tadd %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_SUB:
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, %s, %s\n", dst, b, a);
        break;
    case ANVIL_MIR_OP_MUL:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "mulld" : "mullw", dst, a, b);
        break;
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "divd" : "divw", dst, a, b);
        break;
    case ANVIL_MIR_OP_UDIV:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "divdu" : "divwu", dst, a, b);
        break;
    case ANVIL_MIR_OP_SMOD:
        anvil_strbuf_appendf(&emit->code, "\t%s r11, %s, %s\n", wide ? "divd" : "divw", a, b);
        anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n", wide ? "mulld" : "mullw", b);
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, r11, %s\n", dst, a);
        break;
    case ANVIL_MIR_OP_UMOD:
        anvil_strbuf_appendf(&emit->code, "\t%s r11, %s, %s\n", wide ? "divdu" : "divwu", a, b);
        anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n", wide ? "mulld" : "mullw", b);
        anvil_strbuf_appendf(&emit->code, "\tsubf %s, r11, %s\n", dst, a);
        break;
    case ANVIL_MIR_OP_AND:
        anvil_strbuf_appendf(&emit->code, "\tand %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_OR:
        anvil_strbuf_appendf(&emit->code, "\tor %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_XOR:
        anvil_strbuf_appendf(&emit->code, "\txor %s, %s, %s\n", dst, a, b);
        break;
    case ANVIL_MIR_OP_SHL:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "sld" : "slw", dst, a, b);
        break;
    case ANVIL_MIR_OP_SHR:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "srd" : "srw", dst, a, b);
        break;
    case ANVIL_MIR_OP_SAR:
        anvil_strbuf_appendf(&emit->code, "\t%s %s, %s, %s\n", wide ? "srad" : "sraw", dst, a, b);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static const char *ppc_cmp_branch(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "beq";
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP:
        return "bne";
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_ULT:
        return "blt";
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_ULE:
        return "ble";
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_UGT:
        return "bgt";
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_UGE:
        return "bge";
    default:
        return "bne";
    }
}

static bool ppc_cmp_is_unsigned(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE;
}

static void ppc_emit_cmp(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t lhs;
    anvil_mir_vreg_t rhs;
    if (!ppc_get_uses2(emit->mir, instr_index, &lhs, &rhs)) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *lhs_info = ppc_vreg_info_checked(emit, lhs);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *a = ppc_reg_name(emit, lhs);
    const char *b = ppc_reg_name(emit, rhs);
    if (!lhs_info || emit->failed)
        return;

    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "\tfcmpu 0, %s, %s\n", a, b);
    } else {
        bool wide = ppc_is_64(emit) && lhs_info->size_bits > 32;
        bool unsigned_cmp = ppc_cmp_is_unsigned(info->op);
        anvil_strbuf_appendf(&emit->code, "\t%s 0, %s, %s\n", unsigned_cmp ? (wide ? "cmpld" : "cmplw") : (wide ? "cmpd" : "cmpw"), a, b);
    }

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tli %s, 0\n", dst);
    if (info->op == ANVIL_MIR_OP_FCMP) {
        unsigned mask = 0;
        switch ((anvil_fcmp_pred_t)info->imm) {
        case ANVIL_FCMP_FALSE:
            mask = 0;
            break;
        case ANVIL_FCMP_OEQ:
            mask = 4;
            break;
        case ANVIL_FCMP_OGT:
            mask = 2;
            break;
        case ANVIL_FCMP_OGE:
            mask = 2 | 4;
            break;
        case ANVIL_FCMP_OLT:
            mask = 1;
            break;
        case ANVIL_FCMP_OLE:
            mask = 1 | 4;
            break;
        case ANVIL_FCMP_ONE:
            mask = 1 | 2;
            break;
        case ANVIL_FCMP_ORD:
            mask = 1 | 2 | 4;
            break;
        case ANVIL_FCMP_UEQ:
            mask = 4 | 8;
            break;
        case ANVIL_FCMP_UGT:
            mask = 2 | 8;
            break;
        case ANVIL_FCMP_UGE:
            mask = 2 | 4 | 8;
            break;
        case ANVIL_FCMP_ULT:
            mask = 1 | 8;
            break;
        case ANVIL_FCMP_ULE:
            mask = 1 | 4 | 8;
            break;
        case ANVIL_FCMP_UNE:
            mask = 1 | 2 | 8;
            break;
        case ANVIL_FCMP_UNO:
            mask = 8;
            break;
        case ANVIL_FCMP_TRUE:
            mask = 15;
            break;
        }
        const char *branches[] = {"blt", "bgt", "beq", "bun"};
        for (unsigned bit = 0; bit < 4; bit++)
            if (mask & (1u << bit))
                anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n", branches[bit], anvil_mir_func_name(emit->mir), id);
    } else {
        anvil_strbuf_appendf(&emit->code, "\t%s .L%s_cmp_true_%zu\n", ppc_cmp_branch(info->op), anvil_mir_func_name(emit->mir), id);
    }
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_cmp_done_%zu\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_true_%zu:\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, "\tli %s, 1\n", dst);
    anvil_strbuf_appendf(&emit->code, ".L%s_cmp_done_%zu:\n", anvil_mir_func_name(emit->mir), id);
}

static void ppc_emit_unary(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!def_info || emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        switch (info->op) {
        case ANVIL_MIR_OP_NEG:
            anvil_strbuf_appendf(&emit->code, "\tfneg %s, %s\n", dst, s);
            break;
        case ANVIL_MIR_OP_FABS:
            anvil_strbuf_appendf(&emit->code, "\tfabs %s, %s\n", dst, s);
            break;
        default:
            emit->failed = true;
            break;
        }
        return;
    }

    switch (info->op) {
    case ANVIL_MIR_OP_NEG:
        anvil_strbuf_appendf(&emit->code, "\tneg %s, %s\n", dst, s);
        break;
    case ANVIL_MIR_OP_NOT:
        anvil_strbuf_appendf(&emit->code, "\tnor %s, %s, %s\n", dst, s, s);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_frame_addr(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || info->frame_slot < 0 || (size_t)info->frame_slot >= emit->num_frame_slot_offsets) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    if (!assignment)
        return;
    ppc_emit_addi_large(emit, assignment->phys_reg, 31, -emit->frame_slot_offsets[info->frame_slot]);
}

static void ppc_emit_dyn_alloca(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t count = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (count == ANVIL_MIR_NO_VREG || !info->has_imm || info->imm <= 0) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *dst_assignment = ppc_assignment_checked(emit, info->def);
    const char *count_reg = ppc_reg_name(emit, count);
    if (!dst_assignment || emit->failed)
        return;

    if (info->imm == 1) {
        anvil_strbuf_appendf(&emit->code, "\tmr r11, %s\n", count_reg);
    } else if (info->imm >= -32768 && info->imm <= 32767) {
        anvil_strbuf_appendf(&emit->code, "\tmulli r11, %s, %lld\n", count_reg, (long long)info->imm);
    } else {
        ppc_emit_load_imm(emit, 11, info->imm);
        anvil_strbuf_appendf(&emit->code, "\t%s r11, r11, %s\n", ppc_is_64(emit) ? "mulld" : "mullw", count_reg);
    }
    /* The dynamic frame must expose a complete ABI linkage/outgoing area to
       nested calls.  Keep user payload beyond that aligned prefix. */
    int prefix = (int)emit->desc->outgoing_arg_offset + emit->outgoing_size;
    if (prefix < (int)emit->desc->min_frame_size) {
        prefix = (int)emit->desc->min_frame_size;
    }
    prefix = align_int(prefix, 16);
    if (prefix <= 32752) {
        anvil_strbuf_appendf(&emit->code, "\taddi r11, r11, %d\n", prefix + 15);
    } else {
        ppc_emit_load_imm(emit, 0, prefix + 15);
        anvil_strbuf_append(&emit->code, "\tadd r11, r11, r0\n");
    }
    anvil_strbuf_append(&emit->code, ppc_is_64(emit) ? "\tclrrdi r11, r11, 4\n" : "\trlwinm r11, r11, 0, 0, 27\n");
    anvil_strbuf_append(&emit->code, "\tneg r11, r11\n");
    anvil_strbuf_appendf(&emit->code, "\t%s r1, r1, r11\n", ppc_is_64(emit) ? "stdux" : "stwux");
    ppc_emit_addi_large(emit, dst_assignment->phys_reg, 1, prefix);
}

static void ppc_emit_mov_fpr_imm(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info, const anvil_mir_vreg_info_t *def_info, const char *dst)
{
    if (!emit->has_frame || emit->fp_const_scratch_offset < 0) {
        emit->failed = true;
        return;
    }

    uint64_t raw = (uint64_t)info->imm;
    int size = ppc_mir_size_bytes(def_info->size_bits);
    if (size <= 4) {
        ppc_emit_load_imm(emit, 11, (int32_t)(raw & 0xffffffffu));
        ppc_emit_local_access(emit, "stw", "r11", emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfs", dst, emit->fp_const_scratch_offset);
        return;
    }

    if (ppc_is_64(emit)) {
        ppc_emit_load_imm(emit, 11, (int64_t)raw);
        ppc_emit_local_access(emit, "std", "r11", emit->fp_const_scratch_offset);
        ppc_emit_local_access(emit, "lfd", dst, emit->fp_const_scratch_offset);
        return;
    }

    uint32_t hi = (uint32_t)(raw >> 32);
    uint32_t lo = (uint32_t)(raw & 0xffffffffu);
    ppc_emit_load_imm(emit, 11, (int32_t)hi);
    ppc_emit_local_access(emit, "stw", "r11", emit->fp_const_scratch_offset);
    ppc_emit_load_imm(emit, 11, (int32_t)lo);
    ppc_emit_local_access(emit, "stw", "r11", emit->fp_const_scratch_offset - 4);
    ppc_emit_local_access(emit, "lfd", dst, emit->fp_const_scratch_offset);
}

static void ppc_emit_mov(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || !assignment || emit->failed)
        return;

    if (def_info->reg_class == ANVIL_MIR_REG_FPR) {
        ppc_emit_mov_fpr_imm(emit, info, def_info, dst);
        return;
    }

    ppc_emit_load_imm(emit, assignment->phys_reg, info->imm);
}

static void ppc_emit_symbol_addr(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!info->symbol || !info->symbol[0]) {
        emit->failed = true;
        return;
    }

    const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
    if (!assignment)
        return;
    const char *dst = ppc_gpr_names[assignment->phys_reg];

    if (ppc_is_64(emit)) {
        anvil_strbuf_appendf(&emit->code, "\taddis %s, r2, %s", dst, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@toc@ha\n");
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %s", dst, dst, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@toc@l\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "\tlis %s, %s", dst, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@ha\n");
        anvil_strbuf_appendf(&emit->code, "\taddi %s, %s, %s", dst, dst, info->symbol);
        if (info->has_imm && info->imm != 0)
            anvil_strbuf_appendf(&emit->code, "%+lld", (long long)info->imm);
        anvil_strbuf_append(&emit->code, "@l\n");
    }
}

static void ppc_emit_load(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *base = ppc_reg_name(emit, ptr);
    if (!def_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    const char *op = ppc_load_op(emit->desc, def_info->reg_class, size, def_info->is_signed);
    ppc_emit_base_offset_access(emit, op, dst, base, info->has_imm ? info->imm : 0);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_store(ppc_mir_emit_t *emit, size_t instr_index)
{
    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    if (value == ANVIL_MIR_NO_VREG || ptr == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    const char *base = ppc_reg_name(emit, ptr);
    if (!value_info || emit->failed)
        return;

    anvil_mir_instr_info_t info;
    int64_t offset = 0;
    if (anvil_mir_get_instr_info(emit->mir, instr_index, &info) && info.has_imm) {
        offset = info.imm;
    }

    int size = ppc_mir_size_bytes(value_info->size_bits);
    ppc_emit_base_offset_access(emit, ppc_store_op(emit->desc, value_info->reg_class, size), src, base, offset);
}

static void ppc_emit_incoming_stack_arg(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (!emit->has_frame || !info->has_imm || info->imm < 0 || info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *dst = ppc_reg_name(emit, info->def);
    if (!def_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(def_info->size_bits);
    int frame_offset = (int)emit->desc->incoming_arg_offset + (int)info->imm;
    ppc_emit_incoming_stack_access(emit, ppc_load_op(emit->desc, def_info->reg_class, size, def_info->is_signed), dst, frame_offset);
    ppc_emit_sign_extend_loaded_byte(emit, def_info, dst, size);
}

static void ppc_emit_gpr_extend(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info, bool sign_extend)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    const char *dst = ppc_reg_name(emit, info->def);
    const char *s = ppc_reg_name(emit, src);
    if (!dst_info || !src_info || emit->failed)
        return;
    if (dst_info->reg_class != ANVIL_MIR_REG_GPR || src_info->reg_class != ANVIL_MIR_REG_GPR) {
        emit->failed = true;
        return;
    }

    int src_size = ppc_mir_size_bytes(src_info->size_bits);
    int dst_size = ppc_mir_size_bytes(dst_info->size_bits);
    if (sign_extend) {
        if (src_size <= 1) {
            anvil_strbuf_appendf(&emit->code, "\textsb %s, %s\n", dst, s);
        } else if (src_size <= 2) {
            anvil_strbuf_appendf(&emit->code, "\textsh %s, %s\n", dst, s);
        } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
            anvil_strbuf_appendf(&emit->code, "\textsw %s, %s\n", dst, s);
        } else {
            anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
        }
        return;
    }

    if (src_size <= 1) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 24, 31\n", dst, s);
    } else if (src_size <= 2) {
        anvil_strbuf_appendf(&emit->code, "\trlwinm %s, %s, 0, 16, 31\n", dst, s);
    } else if (ppc_is_64(emit) && dst_size > 4 && src_size <= 4) {
        anvil_strbuf_appendf(&emit->code, "\trldicl %s, %s, 0, 32\n", dst, s);
    } else {
        anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", dst, s);
    }
}

static void ppc_emit_cast(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *dst_info = ppc_vreg_info_checked(emit, info->def);
    const anvil_mir_vreg_info_t *src_info = ppc_vreg_info_checked(emit, src);
    if (!dst_info || !src_info)
        return;

    const char *dst = ppc_reg_name(emit, info->def);
    const char *source = ppc_reg_name(emit, src);
    if (emit->failed)
        return;

    switch (info->op) {
    case ANVIL_MIR_OP_ZEXT:
        ppc_emit_gpr_extend(emit, instr_index, info, false);
        break;
    case ANVIL_MIR_OP_SEXT:
        ppc_emit_gpr_extend(emit, instr_index, info, true);
        break;
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        ppc_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP: {
        bool is_unsigned = info->op == ANVIL_MIR_OP_UITOFP;
        if (src_info->size_bits <= 8) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm r3, %s, 0, 24, 31\n" : "\textsb r3, %s\n", source);
        } else if (src_info->size_bits <= 16) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm r3, %s, 0, 16, 31\n" : "\textsh r3, %s\n", source);
        } else if (src_info->size_bits <= 32 && ppc_is_64(emit)) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trldicl r3, %s, 0, 32\n" : "\textsw r3, %s\n", source);
        } else if (strcmp(source, "r3") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tmr r3, %s\n", source);
        }

        const char *helper;
        if (is_unsigned) {
            helper = src_info->size_bits <= 32 ? (dst_info->size_bits == 32 ? "__floatunsisf" : "__floatunsidf") : (dst_info->size_bits == 32 ? "__floatundisf" : "__floatundidf");
        } else {
            helper = src_info->size_bits <= 32 ? (dst_info->size_bits == 32 ? "__floatsisf" : "__floatsidf") : (dst_info->size_bits == 32 ? "__floatdisf" : "__floatdidf");
        }
        emit->desc->abi_ops->emit_direct_call(emit, helper);
        if (strcmp(dst, "f1") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tfmr %s, f1\n", dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI: {
        bool is_unsigned = info->op == ANVIL_MIR_OP_FPTOUI;
        if (strcmp(source, "f1") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tfmr f1, %s\n", source);
        }
        const char *helper;
        if (is_unsigned) {
            helper = dst_info->size_bits <= 32 ? (src_info->size_bits == 32 ? "__fixunssfsi" : "__fixunsdfsi") : (src_info->size_bits == 32 ? "__fixunssfdi" : "__fixunsdfdi");
        } else {
            helper = dst_info->size_bits <= 32 ? (src_info->size_bits == 32 ? "__fixsfsi" : "__fixdfsi") : (src_info->size_bits == 32 ? "__fixsfdi" : "__fixdfdi");
        }
        emit->desc->abi_ops->emit_direct_call(emit, helper);

        if (dst_info->size_bits <= 8) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, r3, 0, 24, 31\n" : "\textsb %s, r3\n", dst);
        } else if (dst_info->size_bits <= 16) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trlwinm %s, r3, 0, 16, 31\n" : "\textsh %s, r3\n", dst);
        } else if (dst_info->size_bits <= 32 && ppc_is_64(emit)) {
            anvil_strbuf_appendf(&emit->code, is_unsigned ? "\trldicl %s, r3, 0, 32\n" : "\textsw %s, r3\n", dst);
        } else if (strcmp(dst, "r3") != 0) {
            anvil_strbuf_appendf(&emit->code, "\tmr %s, r3\n", dst);
        }
        break;
    }
    case ANVIL_MIR_OP_FPEXT:
        /* PowerPC FPRs hold a loaded f32 as its numeric value in the
         * register's wider representation, so extending it is a move. */
        ppc_emit_copy(emit, info->def, src);
        break;
    case ANVIL_MIR_OP_FPTRUNC:
        anvil_strbuf_appendf(&emit->code, "\tfrsp %s, %s\n", dst, source);
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_select(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    anvil_mir_vreg_t then_v = anvil_mir_get_instr_use(emit->mir, instr_index, 1);
    anvil_mir_vreg_t else_v = anvil_mir_get_instr_use(emit->mir, instr_index, 2);
    if (cond == ANVIL_MIR_NO_VREG || then_v == ANVIL_MIR_NO_VREG || else_v == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *def_info = ppc_vreg_info_checked(emit, info->def);
    const char *cond_reg = ppc_reg_name(emit, cond);
    if (!def_info || emit->failed)
        return;

    size_t id = emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
    anvil_strbuf_appendf(&emit->code, "\tbeq .L%s_select_else_%zu\n", anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, then_v);
    anvil_strbuf_appendf(&emit->code, "\tb .L%s_select_done_%zu\n", anvil_mir_func_name(emit->mir), id);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_else_%zu:\n", anvil_mir_func_name(emit->mir), id);
    ppc_emit_copy(emit, info->def, else_v);
    anvil_strbuf_appendf(&emit->code, ".L%s_select_done_%zu:\n", anvil_mir_func_name(emit->mir), id);
}

static void ppc_emit_call_stack_arg(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (!info->has_imm || info->imm < 0 || info->num_uses != 1 || info->imm > INT32_MAX) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (value == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const anvil_mir_vreg_info_t *value_info = ppc_vreg_info_checked(emit, value);
    const char *src = ppc_reg_name(emit, value);
    if (!value_info || emit->failed)
        return;

    int size = ppc_mir_size_bytes(value_info->size_bits);
    int offset = (int)emit->desc->outgoing_arg_offset + (int)info->imm;
    ppc_emit_sp_access(emit, ppc_store_op(emit->desc, value_info->reg_class, size), src, offset);
}

static void ppc_emit_spill_load(ppc_mir_emit_t *emit, const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 || (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    const char *dst = ppc_reg_name(emit, info->def);
    if (emit->failed)
        return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit, ppc_load_op(emit->desc, slot.reg_class, size, false), dst, emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_spill_store(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->spill_slot < 0 || (size_t)info->spill_slot >= emit->num_spill_offsets) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (src_vreg == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    anvil_mir_spill_slot_info_t slot;
    if (!anvil_mir_get_spill_slot_info(emit->mir, info->spill_slot, &slot)) {
        emit->failed = true;
        return;
    }

    const char *src = ppc_reg_name(emit, src_vreg);
    if (emit->failed)
        return;
    int size = ppc_mir_size_bytes(slot.size_bits);
    ppc_emit_local_access(emit, ppc_store_op(emit->desc, slot.reg_class, size), src, emit->spill_offsets[info->spill_slot]);
}

static void ppc_emit_ret(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses > 0) {
        anvil_mir_vreg_t ret = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        const anvil_mir_vreg_info_t *ret_info = ppc_vreg_info_checked(emit, ret);
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, ret);
        if (!ret_info || !assignment)
            return;

        int ret_reg = ret_info->reg_class == ANVIL_MIR_REG_FPR ? emit->desc->fpr_return_reg : emit->desc->gpr_return_reg;
        if (assignment->phys_reg != ret_reg) {
            if (ret_info->reg_class == ANVIL_MIR_REG_FPR) {
                anvil_strbuf_appendf(&emit->code, "\tfmr %s, %s\n", ppc_fpr_names[ret_reg], ppc_fpr_names[assignment->phys_reg]);
            } else {
                anvil_strbuf_appendf(&emit->code, "\tmr %s, %s\n", ppc_gpr_names[ret_reg], ppc_gpr_names[assignment->phys_reg]);
            }
        }
    }
    if (!emit->failed)
        ppc_emit_epilogue(emit);
}

static void ppc_emit_indirect_call(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    if (info->num_uses == 0) {
        emit->failed = true;
        return;
    }

    anvil_mir_vreg_t target = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
    if (target == ANVIL_MIR_NO_VREG) {
        emit->failed = true;
        return;
    }

    const char *target_reg = ppc_reg_name(emit, target);
    if (emit->failed)
        return;

    emit->desc->abi_ops->emit_indirect_call(emit, target_reg);
}

static void ppc_emit_instr(ppc_mir_emit_t *emit, size_t instr_index, const anvil_mir_instr_info_t *info)
{
    switch (info->op) {
    case ANVIL_MIR_OP_MOV:
        ppc_emit_mov(emit, info);
        break;
    case ANVIL_MIR_OP_COPY: {
        anvil_mir_vreg_t src = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (src == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            break;
        }
        ppc_emit_copy(emit, info->def, src);
        break;
    }
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_FDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        ppc_emit_binary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        ppc_emit_unary(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        ppc_emit_cast(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SELECT:
        ppc_emit_select(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_FCMP:
    case ANVIL_MIR_OP_CMP:
    case ANVIL_MIR_OP_CMP_EQ:
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_ULT:
    case ANVIL_MIR_OP_CMP_ULE:
    case ANVIL_MIR_OP_CMP_UGT:
    case ANVIL_MIR_OP_CMP_UGE:
        ppc_emit_cmp(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        ppc_emit_symbol_addr(emit, info);
        break;
    case ANVIL_MIR_OP_LOAD:
        ppc_emit_load(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_STORE:
        ppc_emit_store(emit, instr_index);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        ppc_emit_frame_addr(emit, info);
        break;
    case ANVIL_MIR_OP_DYN_ALLOCA:
        ppc_emit_dyn_alloca(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        ppc_emit_incoming_stack_arg(emit, info);
        break;
    case ANVIL_MIR_OP_CALL:
        if (info->symbol && info->symbol[0]) {
            emit->desc->abi_ops->emit_direct_call(emit, info->symbol);
        } else {
            ppc_emit_indirect_call(emit, instr_index, info);
        }
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        ppc_emit_call_stack_arg(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_BR:
        anvil_strbuf_append(&emit->code, "\tb ");
        if (!ppc_emit_branch_target(emit, info->true_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n");
        break;
    case ANVIL_MIR_OP_BR_COND: {
        anvil_mir_vreg_t cond = anvil_mir_get_instr_use(emit->mir, instr_index, 0);
        if (cond == ANVIL_MIR_NO_VREG) {
            emit->failed = true;
            break;
        }
        const char *cond_reg = ppc_reg_name(emit, cond);
        if (emit->failed)
            break;
        anvil_strbuf_appendf(&emit->code, "\tcmpwi 0, %s, 0\n", cond_reg);
        anvil_strbuf_append(&emit->code, "\tbne ");
        if (!ppc_emit_branch_target(emit, info->true_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n\tb ");
        if (!ppc_emit_branch_target(emit, info->false_block)) {
            emit->failed = true;
            break;
        }
        anvil_strbuf_append(&emit->code, "\n");
        break;
    }
    case ANVIL_MIR_OP_RET:
        ppc_emit_ret(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD:
        ppc_emit_spill_load(emit, info);
        break;
    case ANVIL_MIR_OP_SPILL_STORE:
        ppc_emit_spill_store(emit, instr_index, info);
        break;
    case ANVIL_MIR_OP_CALL_RESULT: {
        const anvil_regalloc_assignment_t *assignment = ppc_assignment_checked(emit, info->def);
        if (!assignment || assignment->phys_reg != 4 || emit->desc->word_size != 4) {
            emit->failed = true;
        }
    } break;
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        emit->failed = true;
        break;
    default:
        emit->failed = true;
        break;
    }
}

static void ppc_emit_escaped_string(anvil_strbuf_t *code, const char *value)
{
    anvil_strbuf_append(code, "\t.asciz \"");
    for (const char *p = value ? value : ""; *p; p++) {
        switch (*p) {
        case '\n':
            anvil_strbuf_append(code, "\\n");
            break;
        case '\r':
            anvil_strbuf_append(code, "\\r");
            break;
        case '\t':
            anvil_strbuf_append(code, "\\t");
            break;
        case '\\':
            anvil_strbuf_append(code, "\\\\");
            break;
        case '"':
            anvil_strbuf_append(code, "\\\"");
            break;
        default:
            anvil_strbuf_append_char(code, *p);
            break;
        }
    }
    anvil_strbuf_append(code, "\"\n");
}

static void ppc_emit_string_literals(ppc_mir_emit_t *emit)
{
    size_t count = anvil_mir_num_string_literals(emit->mir);
    if (count == 0)
        return;

    anvil_strbuf_append(&emit->code, "\t.section .rodata\n");
    for (size_t i = 0; i < count; i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info) || !info.label) {
            emit->failed = true;
            return;
        }
        anvil_strbuf_appendf(&emit->code, "%s:\n", info.label);
        ppc_emit_escaped_string(&emit->code, info.value);
    }
}

bool anvil_ppc_emit_mir(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, char **output, size_t *len)
{
    if (!output)
        return false;
    *output = NULL;
    if (len)
        *len = 0;

    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !mir)
        return false;
    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0))
        return false;

    ppc_mir_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.desc = desc;
    emit.mir = mir;
    anvil_strbuf_init(&emit.code);

    if (!ppc_prepare_frame(&emit)) {
        anvil_strbuf_destroy(&emit.code);
        free(emit.spill_offsets);
        free(emit.frame_slot_offsets);
        return false;
    }

    ppc_emit_prologue(&emit);

    for (size_t b = 0; b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
        if (!ppc_emit_label(&emit, (anvil_mir_block_t)b)) {
            emit.failed = true;
            break;
        }

        for (size_t instr_index = 0; instr_index < anvil_mir_num_instrs(mir); instr_index++) {
            anvil_mir_instr_info_t instr;
            if (!anvil_mir_get_instr_info(mir, instr_index, &instr)) {
                emit.failed = true;
                break;
            }
            if (instr.block != (anvil_mir_block_t)b)
                continue;
            ppc_emit_instr(&emit, instr_index, &instr);
            if (emit.failed || emit.code.failed)
                break;
        }
        if (emit.failed || emit.code.failed)
            break;
    }

    ppc_emit_string_literals(&emit);

    free(emit.spill_offsets);
    free(emit.frame_slot_offsets);

    if (emit.failed || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}
