#include "../systemz_internal.h"

static const char *systemz_gpr_name(int reg)
{
    static const char *names[] = {"R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
    if (reg < 0 || reg > 15)
        return "R?";
    return names[reg];
}

static const char *systemz_fpr_name(int reg)
{
    static const char *names[] = {"F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15"};
    if (reg < 0 || reg > 15)
        return "F?";
    return names[reg];
}

static const anvil_mir_vreg_info_t *systemz_emit_vreg_info(systemz_emit_t *emit, anvil_mir_vreg_t vreg)
{
    return anvil_mir_get_vreg_info(emit->mir, vreg);
}

static const anvil_regalloc_assignment_t *systemz_assignment(systemz_emit_t *emit, anvil_mir_vreg_t vreg)
{
    return anvil_mir_get_assignment(emit->mir, vreg);
}

static int systemz_phys_reg(systemz_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_regalloc_assignment_t *a = systemz_assignment(emit, vreg);
    return a ? a->phys_reg : 0;
}

static const char *systemz_vreg_reg_name(systemz_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, vreg);
    int reg = systemz_phys_reg(emit, vreg);
    return info && info->reg_class == ANVIL_MIR_REG_FPR ? systemz_fpr_name(reg) : systemz_gpr_name(reg);
}

static int systemz_size_bytes(uint16_t bits)
{
    return bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
}

static int systemz_storage_bytes(uint16_t bits)
{
    return (int)((bits + 7u) / 8u);
}

static int systemz_align_int(int value, int align)
{
    if (align <= 1)
        return value;
    return (value + align - 1) & ~(align - 1);
}

static bool systemz_prepare_frame(systemz_emit_t *emit)
{
    size_t num_frame = anvil_mir_num_frame_slots(emit->mir);
    size_t num_spills = anvil_mir_num_spills(emit->mir);
    size_t num_instrs = anvil_mir_num_instrs(emit->mir);
    emit->frame_offsets = calloc(num_frame ? num_frame : 1, sizeof(int));
    emit->spill_offsets = calloc(num_spills ? num_spills : 1, sizeof(int));
    emit->call_arg_value_offsets = malloc((num_instrs ? num_instrs : 1) * sizeof(*emit->call_arg_value_offsets));
    emit->call_arg_is_last = calloc(num_instrs ? num_instrs : 1, sizeof(*emit->call_arg_is_last));
    emit->call_arg_counts = calloc(num_instrs ? num_instrs : 1, sizeof(*emit->call_arg_counts));
    if (!emit->frame_offsets || !emit->spill_offsets || !emit->call_arg_value_offsets || !emit->call_arg_is_last || !emit->call_arg_counts) {
        return false;
    }
    for (size_t i = 0; i < num_instrs; i++) {
        emit->call_arg_value_offsets[i] = -1;
    }

    int offset = (int)emit->desc->local_area_offset;
    for (size_t i = 0; i < num_frame; i++) {
        anvil_mir_frame_slot_info_t slot;
        if (!anvil_mir_get_frame_slot_info(emit->mir, (int)i, &slot))
            return false;
        int align = slot.align_bytes ? slot.align_bytes : 1;
        offset = systemz_align_int(offset, align);
        emit->frame_offsets[i] = offset;
        offset += systemz_storage_bytes(slot.size_bits);
    }

    for (size_t i = 0; i < num_spills; i++) {
        anvil_mir_spill_slot_info_t spill;
        if (!anvil_mir_get_spill_slot_info(emit->mir, (int)i, &spill))
            return false;
        int size = systemz_size_bytes(spill.size_bits);
        offset = systemz_align_int(offset, size > 8 ? 8 : size);
        emit->spill_offsets[i] = offset;
        offset += size;
    }

    emit->max_call_args = 0;
    int max_call_value_bytes = 0;
    for (size_t i = 0; i < num_instrs; i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(emit->mir, i, &instr))
            return false;
        if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;

        size_t count = 0;
        int value_bytes = 0;
        anvil_mir_block_t block = instr.block;
        size_t last_arg_index = SIZE_MAX;
        for (;;) {
            if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG || instr.block != block)
                return false;
            if (!instr.has_imm || instr.imm != (int64_t)count || instr.num_uses != 1) {
                return false;
            }
            anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, i, 0);
            const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, value);
            if (!info)
                return false;
            int size = systemz_size_bytes(info->size_bits);
            int align = size > 8 ? 8 : size;
            value_bytes = systemz_align_int(value_bytes, align);
            emit->call_arg_value_offsets[i] = value_bytes;
            value_bytes += size;
            count++;
            last_arg_index = i;
            i++;
            /* Use the same preparation classification as bundle verification. */
            while (i < num_instrs) {
                if (!anvil_mir_get_instr_info(emit->mir, i, &instr))
                    return false;
                if (instr.block != block || !systemz_is_call_preparation(&instr))
                    break;
                i++;
            }
            if (i >= num_instrs || !anvil_mir_get_instr_info(emit->mir, i, &instr))
                return false;
            if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG || instr.block != block)
                break;
        }
        if (instr.block != block || instr.op != ANVIL_MIR_OP_CALL || last_arg_index == SIZE_MAX) {
            return false;
        }
        emit->call_arg_is_last[last_arg_index] = true;
        emit->call_arg_counts[i] = count;
        if (count > emit->max_call_args)
            emit->max_call_args = count;
        if (value_bytes > max_call_value_bytes) {
            max_call_value_bytes = value_bytes;
        }
    }

    if (emit->max_call_args > 0) {
        offset = systemz_align_int(offset, (int)emit->desc->word_size);
        emit->outgoing_values_offset = offset;
        offset += max_call_value_bytes;
        offset = systemz_align_int(offset, (int)emit->desc->ptr_size);
        emit->outgoing_param_list_offset = offset;
        offset += (int)(emit->max_call_args * emit->desc->ptr_size);
    } else {
        emit->outgoing_values_offset = -1;
        emit->outgoing_param_list_offset = -1;
    }

    emit->frame_size = systemz_align_int(offset, emit->desc->ptr_size == 8 ? 16 : 8);
    return true;
}

void systemz_emit_load_imm(systemz_emit_t *emit, int reg, int64_t imm)
{
    if (emit->desc->has_64bit_gprs) {
        if (imm >= -32768 && imm <= 32767) {
            anvil_strbuf_appendf(&emit->code, "         LGHI  %-4s,%lld\n", systemz_gpr_name(reg), (long long)imm);
        } else if (imm >= -2147483648LL && imm <= 2147483647LL) {
            anvil_strbuf_appendf(&emit->code, "         LGFI  %-4s,%lld\n", systemz_gpr_name(reg), (long long)imm);
        } else {
            anvil_strbuf_appendf(&emit->code, "         LG    %-4s,=FD'%lld'\n", systemz_gpr_name(reg), (long long)imm);
        }
        return;
    }

    if (imm >= 0 && imm <= 4095) {
        anvil_strbuf_appendf(&emit->code, "         LA    %-4s,%lld\n", systemz_gpr_name(reg), (long long)imm);
    } else {
        anvil_strbuf_appendf(&emit->code, "         L     %-4s,=F'%lld'\n", systemz_gpr_name(reg), (long long)imm);
    }
}

static bool systemz_use_ieee_fp(systemz_emit_t *emit)
{
    return emit->fp_format == ANVIL_FP_IEEE754 || emit->fp_format == ANVIL_FP_HFP_IEEE;
}

static void systemz_emit_load_fp_imm(systemz_emit_t *emit, int reg, uint16_t size_bits, int64_t imm)
{
    if (size_bits == 32) {
        union {
            uint32_t u;
            float f;
        } cvt;
        cvt.u = (uint32_t)imm;
        anvil_strbuf_appendf(&emit->code, "         LE    %-4s,=%s'%g'\n", systemz_fpr_name(reg), systemz_use_ieee_fp(emit) ? "EB" : "E", cvt.f);
    } else {
        union {
            uint64_t u;
            double d;
        } cvt;
        cvt.u = (uint64_t)imm;
        anvil_strbuf_appendf(&emit->code, "         LD    %-4s,=%s'%g'\n", systemz_fpr_name(reg), systemz_use_ieee_fp(emit) ? "DB" : "D", cvt.d);
    }
}

static void systemz_emit_header(systemz_emit_t *emit)
{
    anvil_strbuf_append(&emit->code, "***********************************************************************\n");
    anvil_strbuf_appendf(&emit->code, "*        GENERATED BY ANVIL FOR %s\n", emit->desc->name);
    anvil_strbuf_append(&emit->code, "***********************************************************************\n");
    anvil_strbuf_append(&emit->code, "         CSECT\n");
    anvil_strbuf_appendf(&emit->code, "         AMODE %s\n", emit->desc->amode);
    anvil_strbuf_appendf(&emit->code, "         RMODE %s\n", emit->desc->rmode);
}

static void systemz_block_label(systemz_emit_t *emit, anvil_mir_block_t block, char *out, size_t out_len)
{
    anvil_mir_block_info_t info;
    char block_upper[64];
    if (!anvil_mir_get_block_info(emit->mir, block, &info) || !info.name) {
        snprintf(block_upper, sizeof(block_upper), "B%u", block);
    } else {
        systemz_uppercase(block_upper, info.name, sizeof(block_upper));
    }
    snprintf(out, out_len, "%s_%s", emit->func_label, block_upper);
}

static const char *systemz_load_op(systemz_emit_t *emit, uint16_t size_bits, anvil_mir_reg_class_t reg_class)
{
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size_bits == 32 ? "LE" : "LD";
    if (size_bits <= 8)
        return "IC";
    if (size_bits <= 16)
        return "LH";
    if (size_bits <= 32)
        return "L";
    return emit->desc->has_64bit_gprs ? "LG" : "L";
}

static const char *systemz_store_op(systemz_emit_t *emit, uint16_t size_bits, anvil_mir_reg_class_t reg_class)
{
    if (reg_class == ANVIL_MIR_REG_FPR)
        return size_bits == 32 ? "STE" : "STD";
    if (size_bits <= 8)
        return "STC";
    if (size_bits <= 16)
        return "STH";
    if (size_bits <= 32)
        return "ST";
    return emit->desc->has_64bit_gprs ? "STG" : "ST";
}

static void systemz_emit_narrow_load_prefix(systemz_emit_t *emit, const anvil_mir_vreg_info_t *info, anvil_mir_vreg_t dst)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || info->size_bits > 8)
        return;
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n", emit->desc->has_64bit_gprs ? "XGR" : "XR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, dst));
}

static void systemz_emit_narrow_load_suffix(systemz_emit_t *emit, const anvil_mir_vreg_info_t *info, anvil_mir_vreg_t dst)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || info->size_bits > 16)
        return;
    const char *reg = systemz_vreg_reg_name(emit, dst);
    if (info->size_bits <= 8 && info->is_signed) {
        anvil_strbuf_appendf(&emit->code, "         SLL   %-4s,24\n         SRA   %-4s,24\n", reg, reg);
    } else if (info->size_bits <= 16 && !info->is_signed) {
        anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'0000FFFF'\n", reg);
    }
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         %s %-4s,%s\n", info->is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void systemz_emit_copy(systemz_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, dst);
    if (!info)
        return;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n", info->size_bits == 32 ? "LER" : "LDR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, src));
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, src));
    }
}

static void systemz_emit_unsigned_divmod(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs, bool wide)
{
    const char *divisor = systemz_vreg_reg_name(emit, rhs);
    const char *dividend = systemz_vreg_reg_name(emit, lhs);
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %s    R1,%s\n", wide ? "LGR" : "LR", dividend);
    anvil_strbuf_appendf(&emit->code, "         %s   R0,R0\n", wide ? "XGR" : "XR");

    unsigned bits = wide ? 64u : 32u;
    for (unsigned bit = 0; bit < bits; bit++) {
        char carry_done[SYSTEMZ_INTERNAL_LABEL_CAP];
        char no_input[SYSTEMZ_INTERNAL_LABEL_CAP];
        char do_subtract[SYSTEMZ_INTERNAL_LABEL_CAP];
        char no_subtract[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(carry_done, sizeof(carry_done), "%s_UDIV_C_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(no_input, sizeof(no_input), "%s_UDIV_I_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(do_subtract, sizeof(do_subtract), "%s_UDIV_D_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(no_subtract, sizeof(no_subtract), "%s_UDIV_S_%u_%u", emit->func_label, emit->label_counter, bit);
        if (wide) {
            anvil_strbuf_append(&emit->code, "         XGR   R14,R14\n"
                                             "         LTGR  R0,R0\n");
            anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", carry_done);
            anvil_strbuf_append(&emit->code, "         AGHI  R14,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", carry_done);
            anvil_strbuf_append(&emit->code, "         TMHH  R1,X'8000'\n"
                                             "         SLLG  R0,R0,1\n");
            anvil_strbuf_appendf(&emit->code, "         BZ    %s\n", no_input);
            anvil_strbuf_append(&emit->code, "         AGHI  R0,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", no_input);
            anvil_strbuf_append(&emit->code, "         SLLG  R1,R1,1\n");
            anvil_strbuf_append(&emit->code, "         LTGR  R14,R14\n");
            anvil_strbuf_appendf(&emit->code, "         BNZ   %s\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         CLGR  R0,%s\n", divisor);
            anvil_strbuf_appendf(&emit->code, "         BL    %s\n", no_subtract);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         SLGR  R0,%s\n", divisor);
            anvil_strbuf_append(&emit->code, "         AGHI  R1,1\n");
        } else {
            anvil_strbuf_append(&emit->code, "         XR    R14,R14\n"
                                             "         LTR   R0,R0\n");
            anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", carry_done);
            anvil_strbuf_append(&emit->code, "         LA    R14,1\n");
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", carry_done);
            anvil_strbuf_append(&emit->code, "         SLDL  R0,1\n");
            anvil_strbuf_append(&emit->code, "         LTR   R14,R14\n");
            anvil_strbuf_appendf(&emit->code, "         BNZ   %s\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         CLR   R0,%s\n", divisor);
            anvil_strbuf_appendf(&emit->code, "         BL    %s\n", no_subtract);
            anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", do_subtract);
            anvil_strbuf_appendf(&emit->code, "         SR    R0,%s\n", divisor);
            anvil_strbuf_append(&emit->code, "         LA    R1,1(,R1)\n");
        }
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", no_subtract);
    }
    emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n", wide ? "LGR" : "LR", dst, instr.op == ANVIL_MIR_OP_UMOD ? "R0" : "R1");
}

static void systemz_emit_binary(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info)
        return;
    if (instr.def != lhs)
        systemz_emit_copy(emit, instr.def, lhs);

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        bool f32 = info->size_bits == 32;
        bool ieee = systemz_use_ieee_fp(emit);
        const char *op = NULL;
        switch (instr.op) {
        case ANVIL_MIR_OP_ADD:
            op = ieee ? (f32 ? "AEBR" : "ADBR") : (f32 ? "AER" : "ADR");
            break;
        case ANVIL_MIR_OP_SUB:
            op = ieee ? (f32 ? "SEBR" : "SDBR") : (f32 ? "SER" : "SDR");
            break;
        case ANVIL_MIR_OP_MUL:
            op = ieee ? (f32 ? "MEEBR" : "MDBR") : (f32 ? "MER" : "MDR");
            break;
        case ANVIL_MIR_OP_DIV:
        case ANVIL_MIR_OP_FDIV:
            op = ieee ? (f32 ? "DEBR" : "DDBR") : (f32 ? "DER" : "DDR");
            break;
        default:
            op = "ADR";
            break;
        }
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", op, systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, rhs));
        return;
    }

    bool wide = info->size_bits > 32 && emit->desc->has_64bit_gprs;
    const char *op = NULL;
    switch (instr.op) {
    case ANVIL_MIR_OP_ADD:
        op = wide ? "AGR" : "AR";
        break;
    case ANVIL_MIR_OP_SUB:
        op = wide ? "SGR" : "SR";
        break;
    case ANVIL_MIR_OP_MUL:
        if (wide) {
            op = "MSGR";
            break;
        }
        anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n", systemz_vreg_reg_name(emit, lhs));
        anvil_strbuf_appendf(&emit->code, "         MR    R0,%s\n", systemz_vreg_reg_name(emit, rhs));
        anvil_strbuf_appendf(&emit->code, "         LR    %-4s,R1\n", systemz_vreg_reg_name(emit, instr.def));
        return;
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
        if (instr.op == ANVIL_MIR_OP_UDIV || instr.op == ANVIL_MIR_OP_UMOD) {
            systemz_emit_unsigned_divmod(emit, instr, lhs, rhs, wide);
            return;
        }
        if (wide) {
            anvil_strbuf_appendf(&emit->code, "         LGR   R0,%s\n", systemz_vreg_reg_name(emit, lhs));
            anvil_strbuf_append(&emit->code, "         SRDAG R0,R0,64\n");
            anvil_strbuf_appendf(&emit->code, "         DSGR  R0,%s\n", systemz_vreg_reg_name(emit, rhs));
            anvil_strbuf_appendf(&emit->code, "         LGR   %-4s,%s\n", systemz_vreg_reg_name(emit, instr.def), (instr.op == ANVIL_MIR_OP_SMOD || instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
        } else {
            anvil_strbuf_appendf(&emit->code, "         LR    R0,%s\n", systemz_vreg_reg_name(emit, lhs));
            anvil_strbuf_append(&emit->code, "         SRDA  R0,32\n");
            anvil_strbuf_appendf(&emit->code, "         DR    R0,%s\n", systemz_vreg_reg_name(emit, rhs));
            anvil_strbuf_appendf(&emit->code, "         LR    %-4s,%s\n", systemz_vreg_reg_name(emit, instr.def), (instr.op == ANVIL_MIR_OP_SMOD || instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
        }
        return;
    case ANVIL_MIR_OP_AND:
        op = wide ? "NGR" : "NR";
        break;
    case ANVIL_MIR_OP_OR:
        op = wide ? "OGR" : "OR";
        break;
    case ANVIL_MIR_OP_XOR:
        op = wide ? "XGR" : "XR";
        break;
    default:
        break;
    }
    if (op) {
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", op, systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, rhs));
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SHL || instr.op == ANVIL_MIR_OP_SHR || instr.op == ANVIL_MIR_OP_SAR) {
        const char *shift = instr.op == ANVIL_MIR_OP_SHL ? (wide ? "SLLG" : "SLL") : instr.op == ANVIL_MIR_OP_SHR ? (wide ? "SRLG" : "SRL") : (wide ? "SRAG" : "SRA");
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(%s)\n", shift, systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, rhs));
    }
}

static const char *systemz_cmp_branch(anvil_mir_opcode_t op)
{
    switch (op) {
    case ANVIL_MIR_OP_CMP_EQ:
        return "BE";
    case ANVIL_MIR_OP_CMP_NE:
    case ANVIL_MIR_OP_CMP:
        return "BNE";
    case ANVIL_MIR_OP_CMP_LT:
    case ANVIL_MIR_OP_CMP_ULT:
        return "BL";
    case ANVIL_MIR_OP_CMP_LE:
    case ANVIL_MIR_OP_CMP_ULE:
        return "BNH";
    case ANVIL_MIR_OP_CMP_GT:
    case ANVIL_MIR_OP_CMP_UGT:
        return "BH";
    case ANVIL_MIR_OP_CMP_GE:
    case ANVIL_MIR_OP_CMP_UGE:
        return "BNL";
    default:
        return "BNE";
    }
}

static void systemz_emit_cmp(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *lhs_info = systemz_emit_vreg_info(emit, lhs);
    const anvil_mir_vreg_info_t *dst_info = systemz_emit_vreg_info(emit, instr.def);
    if (!lhs_info || !dst_info)
        return;

    char true_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    snprintf(true_label, sizeof(true_label), "%s_CMP_T_%u", emit->func_label, emit->label_counter);
    snprintf(end_label, sizeof(end_label), "%s_CMP_E_%u", emit->func_label, emit->label_counter++);

    systemz_emit_load_imm(emit, systemz_phys_reg(emit, instr.def), 0);
    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        bool ieee = systemz_use_ieee_fp(emit);
        anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n", lhs_info->size_bits == 32 ? (ieee ? "CEBR" : "CER") : (ieee ? "CDBR" : "CDR"), systemz_vreg_reg_name(emit, lhs),
                             systemz_vreg_reg_name(emit, rhs));
    } else {
        bool wide = lhs_info->size_bits > 32 && emit->desc->has_64bit_gprs;
        const char *cmp = wide ? "CGR" : "CR";
        if (instr.op == ANVIL_MIR_OP_CMP_ULT || instr.op == ANVIL_MIR_OP_CMP_ULE || instr.op == ANVIL_MIR_OP_CMP_UGT || instr.op == ANVIL_MIR_OP_CMP_UGE) {
            cmp = wide ? "CLGR" : "CLR";
        }
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", cmp, systemz_vreg_reg_name(emit, lhs), systemz_vreg_reg_name(emit, rhs));
    }
    if (instr.op == ANVIL_MIR_OP_FCMP) {
        static const unsigned masks[] = {0, 8, 2, 10, 4, 12, 6, 14, 9, 3, 11, 5, 13, 7, 1, 15};
        unsigned pred = (unsigned)instr.imm;
        if (pred > ANVIL_FCMP_TRUE)
            return;
        if (masks[pred])
            anvil_strbuf_appendf(&emit->code, "         BC    %u,%s\n", masks[pred], true_label);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %-5s %s\n", systemz_cmp_branch(instr.op), true_label);
    }
    anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", true_label);
    systemz_emit_load_imm(emit, systemz_phys_reg(emit, instr.def), 1);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
}

static void systemz_emit_unary(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info)
        return;
    if (instr.def != src)
        systemz_emit_copy(emit, instr.def, src);

    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        if (instr.op == ANVIL_MIR_OP_NEG) {
            anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n", info->size_bits == 32 ? "LCER" : "LCDR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        } else if (instr.op == ANVIL_MIR_OP_FABS) {
            anvil_strbuf_appendf(&emit->code, "         %s   %-4s,%s\n", info->size_bits == 32 ? "LPER" : "LPDR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        }
        return;
    }

    switch (instr.op) {
    case ANVIL_MIR_OP_NEG:
        anvil_strbuf_appendf(&emit->code, "         %s    %-4s,%s\n", emit->desc->has_64bit_gprs ? "LCGR" : "LCR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        break;
    case ANVIL_MIR_OP_NOT:
        anvil_strbuf_appendf(&emit->code, "         %s    %-4s,=X'%s'\n", emit->desc->has_64bit_gprs ? "XG" : "X", systemz_vreg_reg_name(emit, instr.def),
                             emit->desc->has_64bit_gprs ? "FFFFFFFFFFFFFFFF" : "FFFFFFFF");
        break;
    default:
        break;
    }
}

static void systemz_emit_signed_int_to_fp(systemz_emit_t *emit, const char *dst, const char *src, uint16_t src_bits, uint16_t dst_bits)
{
    if (!systemz_use_ieee_fp(emit) && (emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370 || emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370_XA)) {
        char positive[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(positive, sizeof(positive), "%s_IHFP_P_%u", emit->func_label, emit->label_counter++);
        anvil_strbuf_appendf(&emit->code,
                             "         LR    R0,%s\n"
                             "         XR    R14,R14\n"
                             "         LTR   R0,R0\n"
                             "         BNM   %s\n"
                             "         LCR   R0,R0\n"
                             "         L     R14,=X'80000000'\n"
                             "%-8s DS    0H\n"
                             "         L     R1,=X'4E000000'\n"
                             "         OR    R1,R14\n"
                             "         ST    R1,%u(,R11)\n"
                             "         ST    R0,%u(,R11)\n"
                             "         LD    %-4s,%u(,R11)\n",
                             src, positive, positive, emit->desc->fp_temp_offset, emit->desc->fp_temp_offset + 4, dst, emit->desc->fp_temp_offset);
        if (dst_bits == 32) {
            anvil_strbuf_appendf(&emit->code, "         LEDR  %-4s,%s\n", dst, dst);
        }
        return;
    }
    const char *op;
    if (systemz_use_ieee_fp(emit)) {
        if (src_bits > 32)
            op = dst_bits == 32 ? "CEGBR" : "CDGBR";
        else
            op = dst_bits == 32 ? "CEFBR" : "CDFBR";
    } else {
        if (src_bits > 32)
            op = dst_bits == 32 ? "CEGR" : "CDGR";
        else
            op = dst_bits == 32 ? "CEFR" : "CDFR";
    }
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", op, dst, src);
}

static void systemz_emit_fp_to_signed_int(systemz_emit_t *emit, const char *dst, const char *src, uint16_t src_bits, uint16_t dst_bits)
{
    if (!systemz_use_ieee_fp(emit) && (emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370 || emit->desc->variant == ANVIL_MAINFRAME_VARIANT_S370_XA)) {
        char less32[SYSTEMZ_INTERNAL_LABEL_CAP];
        char zero[SYSTEMZ_INTERNAL_LABEL_CAP];
        char apply_sign[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end[SYSTEMZ_INTERNAL_LABEL_CAP];
        unsigned label = emit->label_counter++;
        snprintf(less32, sizeof(less32), "%s_HFI_L_%u", emit->func_label, label);
        snprintf(zero, sizeof(zero), "%s_HFI_Z_%u", emit->func_label, label);
        snprintf(apply_sign, sizeof(apply_sign), "%s_HFI_S_%u", emit->func_label, label);
        snprintf(end, sizeof(end), "%s_HFI_E_%u", emit->func_label, label);
        anvil_strbuf_appendf(&emit->code, "         %s  F0,%s\n", src_bits == 32 ? "LDER" : "LDR", src);
        anvil_strbuf_appendf(&emit->code,
                             "         STD   F0,%u(,R11)\n"
                             "         L     R0,%u(,R11)\n"
                             "         LR    R14,R0\n"
                             "         SRL   R0,24\n"
                             "         N     R0,=X'0000007F'\n"
                             "         SLL   R0,2\n"
                             "         LCR   R0,R0\n"
                             "         A     R0,=F'312'\n"
                             "         L     %-4s,%u(,R11)\n"
                             "         N     %-4s,=X'00FFFFFF'\n"
                             "         C     R0,=F'56'\n"
                             "         BNL   %s\n"
                             "         C     R0,=F'32'\n"
                             "         BL    %s\n"
                             "         S     R0,=F'32'\n"
                             "         SRL   %-4s,0(R0)\n"
                             "         B     %s\n"
                             "%-8s DS    0H\n"
                             "         L     R1,%u(,R11)\n"
                             "         SRL   R1,0(R0)\n"
                             "         LCR   R0,R0\n"
                             "         A     R0,=F'32'\n"
                             "         SLL   %-4s,0(R0)\n"
                             "         OR    %-4s,R1\n"
                             "         B     %s\n"
                             "%-8s DS    0H\n"
                             "         XR    %-4s,%s\n"
                             "%-8s DS    0H\n"
                             "         LTR   R14,R14\n"
                             "         BNM   %s\n"
                             "         LCR   %-4s,%s\n"
                             "%-8s DS    0H\n",
                             emit->desc->fp_temp_offset, emit->desc->fp_temp_offset, dst, emit->desc->fp_temp_offset, dst, zero, less32, dst, apply_sign, less32, emit->desc->fp_temp_offset + 4, dst,
                             dst, apply_sign, zero, dst, dst, apply_sign, end, dst, dst, end);
        (void)dst_bits;
        return;
    }
    const char *op;
    if (systemz_use_ieee_fp(emit)) {
        if (dst_bits > 32)
            op = src_bits == 32 ? "CGEBR" : "CGDBR";
        else
            op = src_bits == 32 ? "CFEBR" : "CFDBR";
        /* M3=5 is round toward zero.  The optional M4 field is omitted,
           selecting the architectural default exception behavior. */
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,5,%s\n", op, dst, src);
    } else {
        if (dst_bits > 32)
            op = src_bits == 32 ? "CGER" : "CGDR";
        else
            op = src_bits == 32 ? "CFER" : "CFDR";
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,5,%s\n", op, dst, src);
    }
}

static void systemz_emit_integer_normalize(systemz_emit_t *emit, const char *reg, uint16_t bits, bool is_signed)
{
    if (bits <= 8) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code, "         SLL   %-4s,24\n         SRA   %-4s,24\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'000000FF'\n", reg);
        }
    } else if (bits <= 16) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code, "         SLL   %-4s,16\n         SRA   %-4s,16\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'0000FFFF'\n", reg);
        }
    }
    if (emit->desc->has_64bit_gprs && bits <= 32) {
        anvil_strbuf_appendf(&emit->code, "         %s %-4s,%s\n", is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void systemz_emit_numeric_cast(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t src, const anvil_mir_vreg_info_t *dst_info, const anvil_mir_vreg_info_t *src_info)
{
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    const char *source = systemz_vreg_reg_name(emit, src);
    bool ieee = systemz_use_ieee_fp(emit);

    if (instr.op == ANVIL_MIR_OP_FPEXT || instr.op == ANVIL_MIR_OP_FPTRUNC) {
        const char *op = instr.op == ANVIL_MIR_OP_FPEXT ? (ieee ? "LDEBR" : "LDER") : (ieee ? "LEDBR" : "LEDR");
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", op, dst, source);
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SITOFP || instr.op == ANVIL_MIR_OP_UITOFP) {
        bool is_unsigned = instr.op == ANVIL_MIR_OP_UITOFP;
        const char *integer = source;
        if (src_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
            anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
            systemz_emit_integer_normalize(emit, "R0", src_info->size_bits, !is_unsigned);
            integer = "R0";
            /* A zero-extended u8/u16/u32 fits the wider signed domain. */
            is_unsigned = false;
        }
        if (!is_unsigned) {
            systemz_emit_signed_int_to_fp(emit, dst, integer, src_info->size_bits, dst_info->size_bits);
            return;
        }

        char low_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(low_label, sizeof(low_label), "%s_UIFP_L_%u", emit->func_label, emit->label_counter);
        snprintf(end_label, sizeof(end_label), "%s_UIFP_E_%u", emit->func_label, emit->label_counter++);
        anvil_strbuf_appendf(&emit->code, "         %s  %s,%s\n", emit->desc->has_64bit_gprs ? "LTGR" : "LTR", source, source);
        anvil_strbuf_appendf(&emit->code, "         BNM   %s\n", low_label);
        anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_append(&emit->code, "         SRLG  R0,R0,1\n");
        } else {
            anvil_strbuf_append(&emit->code, "         SRL   R0,1\n");
        }
        anvil_strbuf_appendf(&emit->code, "         %s    R1,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
        anvil_strbuf_append(&emit->code, emit->desc->has_64bit_gprs ? "         NILL  R1,X'0001'\n" : "         N     R1,=X'00000001'\n");
        anvil_strbuf_appendf(&emit->code, "         %s    R0,R1\n", emit->desc->has_64bit_gprs ? "OGR" : "OR");
        systemz_emit_signed_int_to_fp(emit, dst, "R0", src_info->size_bits, dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%s\n", ieee ? (dst_info->size_bits == 32 ? "AEBR" : "ADBR") : (dst_info->size_bits == 32 ? "AER" : "ADR"), dst, dst);
        anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", low_label);
        systemz_emit_signed_int_to_fp(emit, dst, source, src_info->size_bits, dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
        return;
    }

    bool is_unsigned = instr.op == ANVIL_MIR_OP_FPTOUI;
    if (!is_unsigned || dst_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
        systemz_emit_fp_to_signed_int(emit, dst, source, src_info->size_bits, dst_info->size_bits);
        systemz_emit_integer_normalize(emit, dst, dst_info->size_bits, !is_unsigned);
        return;
    }

    char low_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    snprintf(low_label, sizeof(low_label), "%s_FPUI_L_%u", emit->func_label, emit->label_counter);
    snprintf(end_label, sizeof(end_label), "%s_FPUI_E_%u", emit->func_label, emit->label_counter++);
    const char *literal;
    if (src_info->size_bits == 32) {
        literal = ieee ? (dst_info->size_bits > 32 ? "=EB'9223372036854775808'" : "=EB'2147483648'") : (dst_info->size_bits > 32 ? "=E'9223372036854775808'" : "=E'2147483648'");
    } else {
        literal = ieee ? (dst_info->size_bits > 32 ? "=DB'9223372036854775808'" : "=DB'2147483648'") : (dst_info->size_bits > 32 ? "=D'9223372036854775808'" : "=D'2147483648'");
    }
    anvil_strbuf_appendf(&emit->code, "         %s    F0,%s\n", src_info->size_bits == 32 ? "LE" : "LD", literal);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,F0\n", ieee ? (src_info->size_bits == 32 ? "CEBR" : "CDBR") : (src_info->size_bits == 32 ? "CER" : "CDR"), source);
    anvil_strbuf_appendf(&emit->code, "         BL    %s\n", low_label);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,%s\n", ieee ? (src_info->size_bits == 32 ? "SEBR" : "SDBR") : (src_info->size_bits == 32 ? "SER" : "SDR"), source);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,F0\n", ieee ? (src_info->size_bits == 32 ? "LCEBR" : "LCDBR") : (src_info->size_bits == 32 ? "LCER" : "LCDR"));
    systemz_emit_fp_to_signed_int(emit, dst, "F0", src_info->size_bits, dst_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "         %s    %-4s,=X'%s'\n", emit->desc->has_64bit_gprs ? "OG" : "O", dst, emit->desc->has_64bit_gprs ? "8000000000000000" : "80000000");
    anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", low_label);
    systemz_emit_fp_to_signed_int(emit, dst, source, src_info->size_bits, dst_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
}

static void systemz_emit_cast(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *dst_info = systemz_emit_vreg_info(emit, instr.def);
    const anvil_mir_vreg_info_t *src_info = systemz_emit_vreg_info(emit, src);
    if (!dst_info || !src_info)
        return;
    bool numeric_fp_cast = instr.op == ANVIL_MIR_OP_SITOFP || instr.op == ANVIL_MIR_OP_UITOFP || instr.op == ANVIL_MIR_OP_FPTOSI || instr.op == ANVIL_MIR_OP_FPTOUI || instr.op == ANVIL_MIR_OP_FPEXT ||
                           instr.op == ANVIL_MIR_OP_FPTRUNC;
    if (!numeric_fp_cast && dst_info->reg_class == src_info->reg_class) {
        systemz_emit_copy(emit, instr.def, src);
        if (dst_info->reg_class == ANVIL_MIR_REG_GPR && instr.op == ANVIL_MIR_OP_ZEXT && src_info->size_bits < dst_info->size_bits) {
            if (src_info->size_bits == 8) {
                anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'000000FF'\n", systemz_vreg_reg_name(emit, instr.def));
            } else if (src_info->size_bits == 16) {
                anvil_strbuf_appendf(&emit->code, "         N     %-4s,=X'0000FFFF'\n", systemz_vreg_reg_name(emit, instr.def));
            }
        }
        return;
    }
    if (numeric_fp_cast) {
        systemz_emit_numeric_cast(emit, instr, src, dst_info, src_info);
    }
}

static void systemz_emit_mov(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info || !instr.has_imm)
        return;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        systemz_emit_load_fp_imm(emit, systemz_phys_reg(emit, instr.def), info->size_bits, instr.imm);
    } else {
        systemz_emit_load_imm(emit, systemz_phys_reg(emit, instr.def), instr.imm);
    }
}

static void systemz_emit_symbol_addr(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    char upper[96];
    systemz_uppercase(upper, instr.symbol, sizeof(upper));
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LARL  %-4s,%s", systemz_vreg_reg_name(emit, instr.def), upper);
    } else {
        anvil_strbuf_appendf(&emit->code, "         LA    %-4s,%s", systemz_vreg_reg_name(emit, instr.def), upper);
    }
    if (instr.has_imm && instr.imm != 0)
        anvil_strbuf_appendf(&emit->code, "%+lld", (long long)instr.imm);
    anvil_strbuf_append(&emit->code, "\n");
}

static bool systemz_get_uses(const anvil_mir_func_t *mir, size_t index, anvil_mir_vreg_t *u0, anvil_mir_vreg_t *u1, anvil_mir_vreg_t *u2)
{
    anvil_mir_instr_info_t info;
    if (!anvil_mir_get_instr_info(mir, index, &info))
        return false;
    if (info.num_uses > 0 && u0)
        *u0 = anvil_mir_get_instr_use(mir, index, 0);
    if (info.num_uses > 1 && u1)
        *u1 = anvil_mir_get_instr_use(mir, index, 1);
    if (info.num_uses > 2 && u2)
        *u2 = anvil_mir_get_instr_use(mir, index, 2);
    return true;
}

static void systemz_emit_load(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    anvil_mir_vreg_t addr = ANVIL_MIR_NO_VREG;
    systemz_get_uses(emit->mir, index, &addr, NULL, NULL);
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info)
        return;
    systemz_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def),
                         systemz_vreg_reg_name(emit, addr));
    systemz_emit_narrow_load_suffix(emit, info, instr.def);
}

static void systemz_emit_store(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    (void)instr;
    anvil_mir_vreg_t value = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t addr = ANVIL_MIR_NO_VREG;
    systemz_get_uses(emit->mir, index, &value, &addr, NULL);
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, value);
    if (!info)
        return;
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, value), systemz_vreg_reg_name(emit, addr));
}

static void systemz_emit_frame_addr(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    int off = instr.frame_slot >= 0 ? emit->frame_offsets[instr.frame_slot] : 0;
    anvil_strbuf_appendf(&emit->code, "         LA    %-4s,%d(,R11)\n", systemz_vreg_reg_name(emit, instr.def), off);
}

static void systemz_emit_incoming_arg(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info || !instr.has_imm)
        return;
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LG    R0,%lld(,R1)\n", (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         NIHH  R0,X'7FFF'\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "         L     R0,%lld(,R1)\n", (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         N     R0,=X'7FFFFFFF'\n");
    }
    systemz_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,0(,%s)\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def), "R0");
    systemz_emit_narrow_load_suffix(emit, info, instr.def);
}

static void systemz_emit_call_stack_arg(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    if (!instr.has_imm || instr.imm < 0 || emit->outgoing_values_offset < 0)
        return;
    anvil_mir_vreg_t value = ANVIL_MIR_NO_VREG;
    systemz_get_uses(emit->mir, index, &value, NULL, NULL);
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, value);
    if (!info)
        return;

    if (!emit->call_arg_value_offsets || emit->call_arg_value_offsets[index] < 0)
        return;
    int value_off = emit->outgoing_values_offset + emit->call_arg_value_offsets[index];
    int list_off = emit->outgoing_param_list_offset + (int)instr.imm * (int)emit->desc->ptr_size;

    anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, value), value_off);
    anvil_strbuf_appendf(&emit->code, "         LA    R0,%d(,R11)\n", value_off);
    if (emit->call_arg_is_last && emit->call_arg_is_last[index]) {
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_append(&emit->code, "         OIHH  R0,X'8000'\n");
        } else {
            anvil_strbuf_append(&emit->code, "         O     R0,=X'80000000'\n");
        }
    }
    anvil_strbuf_appendf(&emit->code, "         %s    R0,%d(,R11)\n", emit->desc->has_64bit_gprs ? "STG" : "ST", list_off);
}

static void systemz_emit_call(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    (void)index;
    if (emit->outgoing_param_list_offset >= 0 && emit->call_arg_counts && emit->call_arg_counts[index] > 0) {
        anvil_strbuf_appendf(&emit->code, "         LA    R1,%d(,R11)\n", emit->outgoing_param_list_offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    R1,R1\n", emit->desc->has_64bit_gprs ? "XGR" : "XR");
    }
    if (instr.symbol) {
        char upper[96];
        systemz_uppercase(upper, instr.symbol, sizeof(upper));
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_appendf(&emit->code, "         LARL  R15,%s\n", upper);
        } else {
            anvil_strbuf_appendf(&emit->code, "         L     R15,=V(%s)\n", upper);
        }
    } else if (instr.num_uses > 0) {
        anvil_mir_vreg_t callee = anvil_mir_get_instr_use(emit->mir, index, 0);
        anvil_strbuf_appendf(&emit->code, "         %s    R15,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, callee));
    }
    anvil_strbuf_append(&emit->code, "         BALR  R14,R15\n");
}

static void systemz_emit_ret(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    if (instr.num_uses > 0) {
        anvil_mir_vreg_t value = anvil_mir_get_instr_use(emit->mir, index, 0);
        const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, value);
        if (info && info->reg_class == ANVIL_MIR_REG_GPR && systemz_phys_reg(emit, value) != emit->desc->return_gpr) {
            anvil_strbuf_appendf(&emit->code, "         %s    R15,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, value));
        }
    }
    emit->desc->abi_ops->emit_epilogue(emit);
}

static void systemz_emit_dynamic_alloca(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t count)
{
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    const char *src = systemz_vreg_reg_name(emit, count);
    long long elem_size = (long long)instr.imm;
    unsigned local = emit->desc->local_area_offset;

    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LGR   R0,%s\n", src);
        if (elem_size != 1) {
            anvil_strbuf_appendf(&emit->code, "         MSGFI R0,%lld\n", elem_size);
        }
        anvil_strbuf_appendf(&emit->code,
                             "         LGR   %-4s,R13\n"
                             "         AGHI  %-4s,%u\n"
                             "         AGHI  %-4s,15\n"
                             "         NILL  %-4s,X'FFF0'\n",
                             dst, dst, local, dst, dst);
        anvil_strbuf_appendf(&emit->code,
                             "         AGR   R0,%s\n"
                             "         AGHI  R0,15\n"
                             "         NILL  R0,X'FFF0'\n"
                             "         STG   R11,8(,R0)\n"
                             "         STG   R0,16(,R11)\n"
                             "         LGR   R13,R0\n",
                             dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n", src);
        if (elem_size != 1) {
            anvil_strbuf_appendf(&emit->code, "         M     R0,=F'%lld'\n", elem_size);
        }
        anvil_strbuf_append(&emit->code, "         LR    R0,R1\n");
        anvil_strbuf_appendf(&emit->code,
                             "         LR    %-4s,R13\n"
                             "         LA    %-4s,%u(,%s)\n"
                             "         LA    %-4s,15(,%s)\n"
                             "         N     %-4s,=X'FFFFFFF0'\n",
                             dst, dst, local, dst, dst, dst, dst);
        anvil_strbuf_appendf(&emit->code,
                             "         AR    R0,%s\n"
                             "         LA    R0,15(,R0)\n"
                             "         N     R0,=X'FFFFFFF0'\n"
                             "         ST    R11,4(,R0)\n"
                             "         ST    R0,8(,R11)\n"
                             "         LR    R13,R0\n",
                             dst);
    }
}

static void systemz_emit_branch_target(systemz_emit_t *emit, anvil_mir_block_t block)
{
    char label[SYSTEMZ_INTERNAL_LABEL_CAP];
    systemz_block_label(emit, block, label, sizeof(label));
    anvil_strbuf_appendf(&emit->code, "%s", label);
}

static void systemz_emit_instr(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    anvil_mir_vreg_t u0 = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t u1 = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t u2 = ANVIL_MIR_NO_VREG;
    systemz_get_uses(emit->mir, index, &u0, &u1, &u2);

    switch (instr.op) {
    case ANVIL_MIR_OP_MOV:
        systemz_emit_mov(emit, instr);
        break;
    case ANVIL_MIR_OP_COPY:
    case ANVIL_MIR_OP_BITCAST:
        systemz_emit_copy(emit, instr.def, u0);
        break;
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_SDIV:
    case ANVIL_MIR_OP_UDIV:
    case ANVIL_MIR_OP_SMOD:
    case ANVIL_MIR_OP_UMOD:
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
    case ANVIL_MIR_OP_FDIV:
        systemz_emit_binary(emit, instr, u0, u1);
        break;
    case ANVIL_MIR_OP_CMP:
    case ANVIL_MIR_OP_FCMP:
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
        systemz_emit_cmp(emit, instr, u0, u1);
        break;
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
        systemz_emit_unary(emit, instr, u0);
        break;
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        systemz_emit_cast(emit, instr, u0);
        break;
    case ANVIL_MIR_OP_SYMBOL_ADDR:
        systemz_emit_symbol_addr(emit, instr);
        break;
    case ANVIL_MIR_OP_LOAD:
        systemz_emit_load(emit, instr, index);
        break;
    case ANVIL_MIR_OP_STORE:
        systemz_emit_store(emit, instr, index);
        break;
    case ANVIL_MIR_OP_FRAME_ADDR:
        systemz_emit_frame_addr(emit, instr);
        break;
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
        systemz_emit_incoming_arg(emit, instr);
        break;
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        systemz_emit_call_stack_arg(emit, instr, index);
        break;
    case ANVIL_MIR_OP_CALL:
        systemz_emit_call(emit, instr, index);
        break;
    case ANVIL_MIR_OP_BR:
        anvil_strbuf_append(&emit->code, "         B     ");
        systemz_emit_branch_target(emit, instr.true_block);
        anvil_strbuf_append(&emit->code, "\n");
        break;
    case ANVIL_MIR_OP_BR_COND:
        anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, u0));
        anvil_strbuf_appendf(&emit->code, "         %s  R0,R0\n", emit->desc->has_64bit_gprs ? "LTGR" : "LTR");
        anvil_strbuf_append(&emit->code, "         BNE   ");
        systemz_emit_branch_target(emit, instr.true_block);
        anvil_strbuf_append(&emit->code, "\n         B     ");
        systemz_emit_branch_target(emit, instr.false_block);
        anvil_strbuf_append(&emit->code, "\n");
        break;
    case ANVIL_MIR_OP_RET:
        systemz_emit_ret(emit, instr, index);
        break;
    case ANVIL_MIR_OP_SPILL_LOAD: {
        const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
        int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
        if (info) {
            systemz_emit_narrow_load_prefix(emit, info, instr.def);
            anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def), off);
            systemz_emit_narrow_load_suffix(emit, info, instr.def);
        }
        break;
    }
    case ANVIL_MIR_OP_SPILL_STORE: {
        const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, u0);
        int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
        if (info) {
            anvil_strbuf_appendf(&emit->code, "         %-5s %-4s,%d(,R11)\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, u0), off);
        }
        break;
    }
    case ANVIL_MIR_OP_DYN_ALLOCA:
        systemz_emit_dynamic_alloca(emit, instr, u0);
        break;
    case ANVIL_MIR_OP_SELECT: {
        char true_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(true_label, sizeof(true_label), "%s_SEL_T_%u", emit->func_label, emit->label_counter);
        snprintf(end_label, sizeof(end_label), "%s_SEL_E_%u", emit->func_label, emit->label_counter++);
        anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, u0));
        anvil_strbuf_appendf(&emit->code, "         %s  R0,R0\n", emit->desc->has_64bit_gprs ? "LTGR" : "LTR");
        anvil_strbuf_appendf(&emit->code, "         BNE   %s\n", true_label);
        systemz_emit_copy(emit, instr.def, u2);
        anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", true_label);
        systemz_emit_copy(emit, instr.def, u1);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
        break;
    }
    case ANVIL_MIR_OP_KEEPALIVE:
        break;
    case ANVIL_MIR_OP_CALL_RESULT:
    case ANVIL_MIR_OP_RET_VALUE_PART:
    case ANVIL_MIR_OP_INVALID:
    case ANVIL_MIR_OP_VA_START:
    case ANVIL_MIR_OP_ATOMIC:
    case ANVIL_MIR_OP_VECTOR_FADD:
    case ANVIL_MIR_OP_VECTOR_FSUB:
    case ANVIL_MIR_OP_VECTOR_FMUL:
    case ANVIL_MIR_OP_VECTOR_FDIV:
        emit->code.failed = true;
        break;
    }
}

static void systemz_emit_string_literals(systemz_emit_t *emit)
{
    for (size_t i = 0; i < anvil_mir_num_string_literals(emit->mir); i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info))
            continue;
        char upper[96];
        systemz_uppercase(upper, info.label, sizeof(upper));
        const unsigned char *bytes = (const unsigned char *)(info.value ? info.value : "");
        size_t length = strlen((const char *)bytes) + 1;
        size_t offset = 0;
        bool first = true;
        while (offset < length) {
            size_t chunk = length - offset;
            if (chunk > 24)
                chunk = 24;
            if (first)
                anvil_strbuf_appendf(&emit->code, "%-8s DC    X'", upper);
            else
                anvil_strbuf_append(&emit->code, "         DC    X'");
            for (size_t b = 0; b < chunk; b++)
                anvil_strbuf_appendf(&emit->code, "%02X", bytes[offset + b]);
            anvil_strbuf_append(&emit->code, "'\n");
            first = false;
            offset += chunk;
        }
    }
}

bool systemz_emit_mir_ex(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format, char **output, size_t *len)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir || !output)
        return false;
    *output = NULL;
    if (len)
        *len = 0;
    if (!anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0))
        return false;

    systemz_emit_t emit;
    memset(&emit, 0, sizeof(emit));
    emit.desc = desc;
    emit.mir = mir;
    emit.fp_format = fp_format;
    anvil_strbuf_init(&emit.code);
    const char *mir_name = anvil_mir_func_name(mir);
    if (mir_name && strlen(mir_name) >= sizeof(emit.func_label)) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }
    systemz_uppercase(emit.func_label, mir_name, sizeof(emit.func_label));

    bool ok = systemz_prepare_frame(&emit);
    if (ok) {
        systemz_emit_header(&emit);
        desc->abi_ops->emit_prologue(&emit);
        for (size_t b = 0; b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
            if (b != 0) {
                char label[SYSTEMZ_INTERNAL_LABEL_CAP];
                systemz_block_label(&emit, (anvil_mir_block_t)b, label, sizeof(label));
                anvil_strbuf_appendf(&emit.code, "%-8s DS    0H\n", label);
            }
            for (size_t instr_index = 0; instr_index < anvil_mir_num_instrs(mir) && !emit.code.failed; instr_index++) {
                anvil_mir_instr_info_t instr;
                if (!anvil_mir_get_instr_info(mir, instr_index, &instr)) {
                    ok = false;
                    break;
                }
                if (instr.block != (anvil_mir_block_t)b)
                    continue;
                systemz_emit_instr(&emit, instr, instr_index);
            }
            if (!ok || emit.code.failed)
                break;
        }
        if (!emit.code.failed) {
            systemz_emit_string_literals(&emit);
            anvil_strbuf_appendf(&emit.code, "%s_DYN EQU   %d\n", emit.func_label, emit.frame_size);
        }
    }

    free(emit.frame_offsets);
    free(emit.spill_offsets);
    free(emit.call_arg_value_offsets);
    free(emit.call_arg_is_last);
    free(emit.call_arg_counts);
    if (!ok || emit.code.failed) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }

    *output = anvil_strbuf_detach(&emit.code, len);
    return *output != NULL;
}
