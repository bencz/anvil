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

    /* Save areas are doubleword aligned. */
    int offset;
    if (!anvil_mir_layout_frame_slots(emit->mir, (int)emit->desc->local_area_offset, 1, 8, false, emit->frame_offsets, &offset))
        return false;

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
                if (instr.block != block || !systemz_is_call_preparation(emit->mir, i, &instr))
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
            anvil_strbuf_appendf(&emit->code, "         LGHI  %s,%lld\n", systemz_gpr_name(reg), (long long)imm);
        } else if (imm >= -2147483648LL && imm <= 2147483647LL && (emit->features & ANVIL_FEATURE_ZARCH_EIMM)) {
            anvil_strbuf_appendf(&emit->code, "         LGFI  %s,%lld\n", systemz_gpr_name(reg), (long long)imm);
        } else if (imm >= -2147483648LL && imm <= 2147483647LL) {
            /* No extended immediates before the z9-109. */
            anvil_strbuf_appendf(&emit->code, "         LGF   %s,=F'%lld'\n", systemz_gpr_name(reg), (long long)imm);
        } else {
            anvil_strbuf_appendf(&emit->code, "         LG    %s,=FD'%lld'\n", systemz_gpr_name(reg), (long long)imm);
        }
        return;
    }

    if (imm >= 0 && imm <= 4095) {
        anvil_strbuf_appendf(&emit->code, "         LA    %s,%lld\n", systemz_gpr_name(reg), (long long)imm);
    } else {
        anvil_strbuf_appendf(&emit->code, "         L     %s,=F'%lld'\n", systemz_gpr_name(reg), (long long)imm);
    }
}

static bool systemz_use_ieee_fp(systemz_emit_t *emit)
{
    return emit->fp_format == ANVIL_FP_IEEE754 || emit->fp_format == ANVIL_FP_HFP_IEEE;
}

static bool systemz_use_ieee_fp(systemz_emit_t *emit);

/* FP constants as exact bit patterns: the immediate holds the IEEE image,
 * re-encoded for HFP targets (no decimal round trip, NaN/Inf intact). */
static void systemz_emit_load_fp_imm(systemz_emit_t *emit, int reg, uint16_t size_bits, int64_t imm)
{
    uint64_t bits = (uint64_t)imm;
    if (!systemz_use_ieee_fp(emit)) {
        double value;
        if (size_bits == 32) {
            float f;
            uint32_t u = (uint32_t)bits;
            memcpy(&f, &u, sizeof(f));
            value = f;
        } else {
            memcpy(&value, &bits, sizeof(value));
        }
        if (!systemz_hfp_bits(value, size_bits == 32 ? 4 : 8, &bits)) {
            /* HFP has no infinities or NaNs: saturate to the largest
             * magnitude (NaN as positive). */
            uint64_t max = size_bits == 32 ? UINT64_C(0x7fffffff) : UINT64_C(0x7fffffffffffffff);
            uint64_t sign = size_bits == 32 ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);
            bits = value < 0 ? (max | sign) : max;
        }
    }
    if (size_bits == 32)
        anvil_strbuf_appendf(&emit->code, "         LE    %s,=X'%08X'\n", systemz_fpr_name(reg), (unsigned)(uint32_t)bits);
    else
        anvil_strbuf_appendf(&emit->code, "         LD    %s,=XL8'%016llX'\n", systemz_fpr_name(reg), (unsigned long long)bits);
}

static void systemz_emit_header(systemz_emit_t *emit)
{
    /* The module's control section, equates and bindings come from
     * emit_module_begin. */
    anvil_strbuf_appendf(&emit->code, "*        FUNCTION %s (%s)\n", emit->func_label, emit->desc->name);
}

/* Block labels: the function's name, '#', the block index (source block
 * names can be long or collide once uppercased). */
static void systemz_block_label(systemz_emit_t *emit, anvil_mir_block_t block, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s#B%u", emit->func_label, (unsigned)block);
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
    anvil_strbuf_appendf(&emit->code, "         %s    %s,%s\n", emit->desc->has_64bit_gprs ? "XGR" : "XR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, dst));
}

static void systemz_emit_narrow_load_suffix(systemz_emit_t *emit, const anvil_mir_vreg_info_t *info, anvil_mir_vreg_t dst)
{
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR || info->size_bits > 16)
        return;
    const char *reg = systemz_vreg_reg_name(emit, dst);
    if (info->size_bits <= 8 && info->is_signed) {
        anvil_strbuf_appendf(&emit->code, "         SLL   %s,24\n         SRA   %s,24\n", reg, reg);
    } else if (info->size_bits <= 16 && !info->is_signed) {
        anvil_strbuf_appendf(&emit->code, "         N     %s,=X'0000FFFF'\n", reg);
    }
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         %s %s,%s\n", info->is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void systemz_emit_copy(systemz_emit_t *emit, anvil_mir_vreg_t dst, anvil_mir_vreg_t src)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, dst);
    if (!info)
        return;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_strbuf_appendf(&emit->code, "         %s   %s,%s\n", info->size_bits == 32 ? "LER" : "LDR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, src));
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    %s,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, dst), systemz_vreg_reg_name(emit, src));
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
        snprintf(carry_done, sizeof(carry_done), "%s#UDIV_C_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(no_input, sizeof(no_input), "%s#UDIV_I_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(do_subtract, sizeof(do_subtract), "%s#UDIV_D_%u_%u", emit->func_label, emit->label_counter, bit);
        snprintf(no_subtract, sizeof(no_subtract), "%s#UDIV_S_%u_%u", emit->func_label, emit->label_counter, bit);
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
            /* Bit 0 is free after the shift; LA would clear the high bits
             * in 24/31-bit mode. */
            anvil_strbuf_append(&emit->code, "         O     R1,=F'1'\n");
        }
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", no_subtract);
    }
    emit->label_counter++;
    anvil_strbuf_appendf(&emit->code, "         %s    %s,%s\n", wide ? "LGR" : "LR", dst, instr.op == ANVIL_MIR_OP_UMOD ? "R0" : "R1");
}

/* Immediate operand forms, by CPU facility: AHI/MHI/CHI need the
 * relative-and-immediate instructions (S/390 G1+), NILF/OILF/XILF the
 * extended immediates (z9); literal-pool operands (=F'n') and displacement
 * shift counts work on every model. */
static bool systemz_operand_wide(const systemz_emit_t *emit, anvil_mir_vreg_t vreg)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(emit->mir, vreg);
    return info && info->size_bits > 32 && emit->desc->has_64bit_gprs;
}

static bool systemz_simm16(int64_t value)
{
    return value >= -32768 && value <= 32767;
}

static int systemz_pow2_shift(int64_t value, bool wide)
{
    if (value <= 0 || (value & (value - 1)) != 0)
        return -1;
    int shift = 0;
    while ((INT64_C(1) << shift) != value)
        shift++;
    return shift < (wide ? 64 : 32) ? shift : -1;
}

static bool systemz_cmp_unsigned(anvil_mir_opcode_t op)
{
    return op == ANVIL_MIR_OP_CMP_ULT || op == ANVIL_MIR_OP_CMP_ULE || op == ANVIL_MIR_OP_CMP_UGT || op == ANVIL_MIR_OP_CMP_UGE;
}

/* Mirrors the forms chosen by systemz_emit_binary and systemz_emit_cmp. */
static bool systemz_takes_immediate(void *ctx, size_t index, const anvil_mir_instr_info_t *info, size_t use, int64_t value)
{
    const systemz_emit_t *emit = ctx;
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(emit->mir, info->def) : NULL;
    if (!def || def->reg_class != ANVIL_MIR_REG_GPR || info->num_uses != 2 || use != 1)
        return false;
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    bool wide = systemz_operand_wide(emit, lhs);
    int64_t v = wide ? value : (int64_t)(int32_t)value;
    bool immrel = (emit->features & ANVIL_FEATURE_ZARCH_IMMREL) != 0;
    switch (info->op) {
    case ANVIL_MIR_OP_ADD:
        return immrel && systemz_simm16(v);
    case ANVIL_MIR_OP_SUB:
        return immrel && systemz_simm16(-v);
    case ANVIL_MIR_OP_MUL:
        return systemz_pow2_shift(v, wide) >= 0 || (immrel && systemz_simm16(v));
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR:
        return !wide;
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR:
        return true;
    default: {
        const anvil_mir_vreg_info_t *lhs_info = anvil_mir_get_vreg_info(emit->mir, lhs);
        if (!anvil_mir_isel_is_compare(info->op) || !lhs_info || lhs_info->reg_class != ANVIL_MIR_REG_GPR)
            return false;
        if (!systemz_cmp_unsigned(info->op) && immrel && systemz_simm16(v))
            return true;
        return !wide;
    }
    }
}

static bool systemz_rhs_immediate(const systemz_emit_t *emit, size_t index, const anvil_mir_instr_info_t *info, int64_t *out)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(emit->mir, index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(emit->mir, index, 1);
    int64_t value;
    if (!anvil_mir_isel_constant(&emit->isel, rhs, &value) || !systemz_takes_immediate((void *)emit, index, info, 1, value))
        return false;
    *out = systemz_operand_wide(emit, lhs) ? value : (int64_t)(int32_t)value;
    return true;
}

/* dst = lhs OP imm, two-address. */
static bool systemz_emit_binary_imm(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index, anvil_mir_vreg_t lhs)
{
    int64_t imm;
    if (!systemz_rhs_immediate(emit, index, &instr, &imm))
        return false;
    bool wide = systemz_operand_wide(emit, lhs);
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    if (systemz_phys_reg(emit, instr.def) != systemz_phys_reg(emit, lhs))
        systemz_emit_copy(emit, instr.def, lhs);
    switch (instr.op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld\n", wide ? "AGHI" : "AHI", dst,
                             (long long)(instr.op == ANVIL_MIR_OP_ADD ? imm : -imm));
        break;
    case ANVIL_MIR_OP_MUL: {
        int shift = systemz_pow2_shift(imm, wide);
        if (shift > 0)
        {
            if (wide)
                anvil_strbuf_appendf(&emit->code, "         SLLG  %s,%s,%d\n", dst, dst, shift);
            else
                anvil_strbuf_appendf(&emit->code, "         SLL   %s,%d\n", dst, shift);
        }
        else if (shift < 0)
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld\n", wide ? "MGHI" : "MHI", dst, (long long)imm);
        break;
    }
    case ANVIL_MIR_OP_AND:
    case ANVIL_MIR_OP_OR:
    case ANVIL_MIR_OP_XOR: {
        const char *eimm = instr.op == ANVIL_MIR_OP_AND ? "NILF" : instr.op == ANVIL_MIR_OP_OR ? "OILF" : "XILF";
        const char *literal = instr.op == ANVIL_MIR_OP_AND ? "N" : instr.op == ANVIL_MIR_OP_OR ? "O" : "X";
        if (emit->features & ANVIL_FEATURE_ZARCH_EIMM)
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld\n", eimm, dst, (long long)(uint32_t)imm);
        else
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,=F'%lld'\n", literal, dst, (long long)imm);
        break;
    }
    case ANVIL_MIR_OP_SHL:
    case ANVIL_MIR_OP_SHR:
    case ANVIL_MIR_OP_SAR: {
        int count = (int)(imm & (wide ? 63 : 31));
        const char *op = instr.op == ANVIL_MIR_OP_SHL ? (wide ? "SLLG" : "SLL") : instr.op == ANVIL_MIR_OP_SHR ? (wide ? "SRLG" : "SRL") : (wide ? "SRAG" : "SRA");
        if (wide)
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s,%d\n", op, dst, dst, count);
        else
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%d\n", op, dst, count);
        break;
    }
    default:
        return false;
    }
    return true;
}

static void systemz_emit_binary(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info)
        return;
    /* Two-address forms compute in the destination, which first receives
     * lhs.  32-bit multiply and division work in the R0:R1 pair instead. */
    bool fpr = info->reg_class == ANVIL_MIR_REG_FPR;
    bool wide_gpr = !fpr && info->size_bits > 32 && emit->desc->has_64bit_gprs;
    bool shift = instr.op == ANVIL_MIR_OP_SHL || instr.op == ANVIL_MIR_OP_SHR || instr.op == ANVIL_MIR_OP_SAR;
    bool commutative = instr.op == ANVIL_MIR_OP_ADD || instr.op == ANVIL_MIR_OP_MUL || instr.op == ANVIL_MIR_OP_AND ||
                       instr.op == ANVIL_MIR_OP_OR || instr.op == ANVIL_MIR_OP_XOR;
    bool in_place = fpr || shift || instr.op == ANVIL_MIR_OP_ADD || instr.op == ANVIL_MIR_OP_SUB ||
                    instr.op == ANVIL_MIR_OP_AND || instr.op == ANVIL_MIR_OP_OR || instr.op == ANVIL_MIR_OP_XOR ||
                    (instr.op == ANVIL_MIR_OP_MUL && wide_gpr);
    const char *rhs_name = systemz_vreg_reg_name(emit, rhs);
    if (in_place) {
        int def_reg = systemz_phys_reg(emit, instr.def);
        if (def_reg == systemz_phys_reg(emit, rhs) && def_reg != systemz_phys_reg(emit, lhs)) {
            /* Copying lhs into the destination would destroy rhs. */
            if (commutative) {
                anvil_mir_vreg_t other = lhs;
                lhs = rhs;
                rhs = other;
                rhs_name = systemz_vreg_reg_name(emit, rhs);
            } else if (fpr) {
                anvil_strbuf_appendf(&emit->code, "         %s   F0,%s\n", info->size_bits == 32 ? "LER" : "LDR", rhs_name);
                rhs_name = "F0";
            } else {
                /* Shift counts are base registers, where R0 means none. */
                const char *temp = shift ? "R1" : "R0";
                anvil_strbuf_appendf(&emit->code, "         %s    %s,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", temp, rhs_name);
                rhs_name = temp;
            }
        }
        if (systemz_phys_reg(emit, instr.def) != systemz_phys_reg(emit, lhs))
            systemz_emit_copy(emit, instr.def, lhs);
    }

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
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", op, systemz_vreg_reg_name(emit, instr.def), rhs_name);
        return;
    }

    bool wide = info->size_bits > 32 && emit->desc->has_64bit_gprs;
    const char *op = NULL;
    switch (instr.op) {
    case ANVIL_MIR_OP_ADD:
        /* Logical forms: same result modulo 2^n, never a fixed-point
         * overflow interrupt (word-pair arithmetic relies on wrapping). */
        op = wide ? "ALGR" : "ALR";
        break;
    case ANVIL_MIR_OP_SUB:
        op = wide ? "SLGR" : "SLR";
        break;
    case ANVIL_MIR_OP_MUL:
        if (wide) {
            op = "MSGR";
            break;
        }
        anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n", systemz_vreg_reg_name(emit, lhs));
        anvil_strbuf_appendf(&emit->code, "         MR    R0,%s\n", systemz_vreg_reg_name(emit, rhs));
        anvil_strbuf_appendf(&emit->code, "         LR    %s,R1\n", systemz_vreg_reg_name(emit, instr.def));
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
            /* DSGR divides the odd register of the pair. */
            anvil_strbuf_appendf(&emit->code, "         LGR   R1,%s\n", systemz_vreg_reg_name(emit, lhs));
            anvil_strbuf_appendf(&emit->code, "         DSGR  R0,%s\n", systemz_vreg_reg_name(emit, rhs));
            anvil_strbuf_appendf(&emit->code, "         LGR   %s,%s\n", systemz_vreg_reg_name(emit, instr.def), (instr.op == ANVIL_MIR_OP_SMOD || instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
        } else {
            anvil_strbuf_appendf(&emit->code, "         LR    R0,%s\n", systemz_vreg_reg_name(emit, lhs));
            anvil_strbuf_append(&emit->code, "         SRDA  R0,32\n");
            anvil_strbuf_appendf(&emit->code, "         DR    R0,%s\n", systemz_vreg_reg_name(emit, rhs));
            anvil_strbuf_appendf(&emit->code, "         LR    %s,%s\n", systemz_vreg_reg_name(emit, instr.def), (instr.op == ANVIL_MIR_OP_SMOD || instr.op == ANVIL_MIR_OP_UMOD) ? "R0" : "R1");
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
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", op, systemz_vreg_reg_name(emit, instr.def), rhs_name);
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SHL || instr.op == ANVIL_MIR_OP_SHR || instr.op == ANVIL_MIR_OP_SAR) {
        const char *shift_op = instr.op == ANVIL_MIR_OP_SHL ? (wide ? "SLLG" : "SLL") : instr.op == ANVIL_MIR_OP_SHR ? (wide ? "SRLG" : "SRL") : (wide ? "SRAG" : "SRA");
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,0(%s)\n", shift_op, systemz_vreg_reg_name(emit, instr.def), rhs_name);
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

static void systemz_emit_cmp(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index, anvil_mir_vreg_t lhs, anvil_mir_vreg_t rhs)
{
    const anvil_mir_vreg_info_t *lhs_info = systemz_emit_vreg_info(emit, lhs);
    const anvil_mir_vreg_info_t *dst_info = systemz_emit_vreg_info(emit, instr.def);
    if (!lhs_info || !dst_info)
        return;

    char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    snprintf(end_label, sizeof(end_label), "%s#CMP_E_%u", emit->func_label, emit->label_counter++);

    /* Compare before writing the result, which may share a register with an
     * operand; LA leaves the condition code alone. */
    if (lhs_info->reg_class == ANVIL_MIR_REG_FPR) {
        bool ieee = systemz_use_ieee_fp(emit);
        anvil_strbuf_appendf(&emit->code, "         %s   %s,%s\n", lhs_info->size_bits == 32 ? (ieee ? "CEBR" : "CER") : (ieee ? "CDBR" : "CDR"), systemz_vreg_reg_name(emit, lhs),
                             systemz_vreg_reg_name(emit, rhs));
    } else {
        bool wide = lhs_info->size_bits > 32 && emit->desc->has_64bit_gprs;
        bool unsigned_cmp = systemz_cmp_unsigned(instr.op);
        const char *cmp = unsigned_cmp ? (wide ? "CLGR" : "CLR") : (wide ? "CGR" : "CR");
        int64_t imm;
        const char *a = systemz_vreg_reg_name(emit, lhs);
        if (systemz_rhs_immediate(emit, index, &instr, &imm)) {
            if (!unsigned_cmp && (emit->features & ANVIL_FEATURE_ZARCH_IMMREL) && systemz_simm16(imm))
                anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld\n", wide ? "CGHI" : "CHI", a, (long long)imm);
            else
                anvil_strbuf_appendf(&emit->code, "         %-5s %s,=F'%lld'\n", unsigned_cmp ? "CL" : "C", a, (long long)imm);
        } else {
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", cmp, a, systemz_vreg_reg_name(emit, rhs));
        }
        if (anvil_mir_isel_feeds_next_branch(&emit->isel, index, &instr)) {
            emit->fused_branch = systemz_cmp_branch(instr.op);
            return;
        }
    }
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    anvil_strbuf_appendf(&emit->code, "         LA    %s,1\n", dst);
    if (instr.op == ANVIL_MIR_OP_FCMP) {
        static const unsigned masks[] = {0, 8, 2, 10, 4, 12, 6, 14, 9, 3, 11, 5, 13, 7, 1, 15};
        unsigned pred = (unsigned)instr.imm;
        if (pred > ANVIL_FCMP_TRUE)
            return;
        if (masks[pred])
            anvil_strbuf_appendf(&emit->code, "         BC    %u,%s\n", masks[pred], end_label);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %-5s %s\n", systemz_cmp_branch(instr.op), end_label);
    }
    anvil_strbuf_appendf(&emit->code, "         LA    %s,0\n", dst);
    anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
}

static void systemz_emit_branch_target(systemz_emit_t *emit, anvil_mir_block_t block);

static const char *systemz_inverse_branch(const char *branch)
{
    static const char *const pairs[][2] = {{"BE", "BNE"}, {"BL", "BNL"}, {"BH", "BNH"}};
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        if (strcmp(branch, pairs[i][0]) == 0)
            return pairs[i][1];
        if (strcmp(branch, pairs[i][1]) == 0)
            return pairs[i][0];
    }
    return NULL;
}

/* Branch on the condition code; a target that is the next block falls
 * through. */
static void systemz_emit_cond_branch(systemz_emit_t *emit, const char *taken, anvil_mir_block_t true_block, anvil_mir_block_t false_block)
{
    const char *inverse = systemz_inverse_branch(taken);
    if (true_block == emit->block + 1 && inverse) {
        anvil_strbuf_appendf(&emit->code, "         %-5s ", inverse);
        systemz_emit_branch_target(emit, false_block);
        anvil_strbuf_append(&emit->code, "\n");
        return;
    }
    anvil_strbuf_appendf(&emit->code, "         %-5s ", taken);
    systemz_emit_branch_target(emit, true_block);
    anvil_strbuf_append(&emit->code, "\n");
    if (false_block != emit->block + 1) {
        anvil_strbuf_append(&emit->code, "         B     ");
        systemz_emit_branch_target(emit, false_block);
        anvil_strbuf_append(&emit->code, "\n");
    }
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
            anvil_strbuf_appendf(&emit->code, "         %s   %s,%s\n", info->size_bits == 32 ? "LCER" : "LCDR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        } else if (instr.op == ANVIL_MIR_OP_FABS) {
            anvil_strbuf_appendf(&emit->code, "         %s   %s,%s\n", info->size_bits == 32 ? "LPER" : "LPDR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        }
        return;
    }

    switch (instr.op) {
    case ANVIL_MIR_OP_NEG:
        anvil_strbuf_appendf(&emit->code, "         %s    %s,%s\n", emit->desc->has_64bit_gprs ? "LCGR" : "LCR", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, instr.def));
        break;
    case ANVIL_MIR_OP_NOT:
        anvil_strbuf_appendf(&emit->code, "         %s    %s,=X'%s'\n", emit->desc->has_64bit_gprs ? "XG" : "X", systemz_vreg_reg_name(emit, instr.def),
                             emit->desc->has_64bit_gprs ? "FFFFFFFFFFFFFFFF" : "FFFFFFFF");
        break;
    default:
        break;
    }
}

static void systemz_emit_signed_int_to_fp(systemz_emit_t *emit, const char *dst, const char *src, uint16_t src_bits, uint16_t dst_bits)
{
    /* Without the HFP-extension facility (S/370, S/390 G1-G4) HFP and
     * fixed point convert through the classic unnormalized-add sequence. */
    if (!systemz_use_ieee_fp(emit) && !(emit->features & ANVIL_FEATURE_ZARCH_HFP_EXT)) {
        char positive[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(positive, sizeof(positive), "%s#IHFP_P_%u", emit->func_label, emit->label_counter++);
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
                             "         LD    %s,%u(,R11)\n",
                             src, positive, positive, emit->desc->fp_temp_offset, emit->desc->fp_temp_offset + 4, dst, emit->desc->fp_temp_offset);
        /* The value is unnormalized (exponent X'4E'); a normalizing add of
         * zero fixes it before any rounding to short. */
        anvil_strbuf_appendf(&emit->code, "         AD    %s,=XL8'0000000000000000'\n", dst);
        if (dst_bits == 32) {
            anvil_strbuf_appendf(&emit->code, "         LEDR  %s,%s\n", dst, dst);
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
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", op, dst, src);
}

static void systemz_emit_fp_to_signed_int(systemz_emit_t *emit, const char *dst, const char *src, uint16_t src_bits, uint16_t dst_bits)
{
    /* Without the HFP-extension facility (S/370, S/390 G1-G4) HFP and
     * fixed point convert through the classic unnormalized-add sequence. */
    if (!systemz_use_ieee_fp(emit) && !(emit->features & ANVIL_FEATURE_ZARCH_HFP_EXT)) {
        char less32[SYSTEMZ_INTERNAL_LABEL_CAP];
        char zero[SYSTEMZ_INTERNAL_LABEL_CAP];
        char apply_sign[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end[SYSTEMZ_INTERNAL_LABEL_CAP];
        unsigned label = emit->label_counter++;
        snprintf(less32, sizeof(less32), "%s#HFI_L_%u", emit->func_label, label);
        snprintf(zero, sizeof(zero), "%s#HFI_Z_%u", emit->func_label, label);
        snprintf(apply_sign, sizeof(apply_sign), "%s#HFI_S_%u", emit->func_label, label);
        snprintf(end, sizeof(end), "%s#HFI_E_%u", emit->func_label, label);
        /* Shift counts travel in R14: base register 0 would mean no base.
         * The sign is tested in memory at the end. */
        unsigned off = emit->desc->fp_temp_offset;
        if (src_bits == 32)
            anvil_strbuf_appendf(&emit->code, "         SDR   F0,F0\n         LER   F0,%s\n", src);
        else
            anvil_strbuf_appendf(&emit->code, "         LDR   F0,%s\n", src);
        anvil_strbuf_appendf(&emit->code,
                             "         STD   F0,%u(,R11)\n"
                             "         L     R0,%u(,R11)\n"
                             "         SRL   R0,24\n"
                             "         N     R0,=X'0000007F'\n"
                             "         SLL   R0,2\n"
                             "         LCR   R0,R0\n"
                             "         A     R0,=F'312'\n"
                             "         L     %s,%u(,R11)\n"
                             "         N     %s,=X'00FFFFFF'\n"
                             "         C     R0,=F'56'\n"
                             "         BNL   %s\n"
                             "         C     R0,=F'32'\n"
                             "         BL    %s\n"
                             "         S     R0,=F'32'\n"
                             "         LR    R14,R0\n"
                             "         SRL   %s,0(R14)\n"
                             "         B     %s\n",
                             off, off, dst, off, dst, zero, less32, dst, apply_sign);
        anvil_strbuf_appendf(&emit->code,
                             "%-8s DS    0H\n"
                             "         L     R1,%u(,R11)\n"
                             "         LR    R14,R0\n"
                             "         SRL   R1,0(R14)\n"
                             "         LCR   R14,R0\n"
                             "         A     R14,=F'32'\n"
                             "         SLL   %s,0(R14)\n"
                             "         OR    %s,R1\n"
                             "         B     %s\n"
                             "%-8s DS    0H\n"
                             "         XR    %s,%s\n"
                             "%-8s DS    0H\n"
                             "         TM    %u(,R11),X'80'\n"
                             "         BNO   %s\n"
                             "         LCR   %s,%s\n"
                             "%-8s DS    0H\n",
                             less32, off + 4, dst, dst, apply_sign, zero, dst, dst, apply_sign, off, end, dst, dst, end);
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
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,5,%s\n", op, dst, src);
    } else {
        if (dst_bits > 32)
            op = src_bits == 32 ? "CGER" : "CGDR";
        else
            op = src_bits == 32 ? "CFER" : "CFDR";
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,5,%s\n", op, dst, src);
    }
}

static void systemz_emit_integer_normalize(systemz_emit_t *emit, const char *reg, uint16_t bits, bool is_signed)
{
    if (bits <= 8) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code, "         SLL   %s,24\n         SRA   %s,24\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code, "         N     %s,=X'000000FF'\n", reg);
        }
    } else if (bits <= 16) {
        if (is_signed) {
            anvil_strbuf_appendf(&emit->code, "         SLL   %s,16\n         SRA   %s,16\n", reg, reg);
        } else {
            anvil_strbuf_appendf(&emit->code, "         N     %s,=X'0000FFFF'\n", reg);
        }
    }
    if (emit->desc->has_64bit_gprs && bits <= 32) {
        anvil_strbuf_appendf(&emit->code, "         %s %s,%s\n", is_signed ? "LGFR " : "LLGFR", reg, reg);
    }
}

static void systemz_emit_numeric_cast(systemz_emit_t *emit, anvil_mir_instr_info_t instr, anvil_mir_vreg_t src, const anvil_mir_vreg_info_t *dst_info, const anvil_mir_vreg_info_t *src_info)
{
    const char *dst = systemz_vreg_reg_name(emit, instr.def);
    const char *source = systemz_vreg_reg_name(emit, src);
    bool ieee = systemz_use_ieee_fp(emit);

    if (instr.op == ANVIL_MIR_OP_FPEXT && !ieee && !(emit->features & ANVIL_FEATURE_ZARCH_HFP_EXT)) {
        /* No LOAD LENGTHENED before the HFP extensions: clear the long
         * register, then load the short value into its high half. */
        if (strcmp(dst, source) == 0) {
            anvil_strbuf_appendf(&emit->code, "         LER   F0,%s\n         SDR   %s,%s\n         LER   %s,F0\n", source, dst, dst, dst);
        } else {
            anvil_strbuf_appendf(&emit->code, "         SDR   %s,%s\n         LER   %s,%s\n", dst, dst, dst, source);
        }
        return;
    }
    if (instr.op == ANVIL_MIR_OP_FPEXT || instr.op == ANVIL_MIR_OP_FPTRUNC) {
        const char *op = instr.op == ANVIL_MIR_OP_FPEXT ? (ieee ? "LDEBR" : "LDER") : (ieee ? "LEDBR" : "LEDR");
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", op, dst, source);
        return;
    }

    if (instr.op == ANVIL_MIR_OP_SITOFP || instr.op == ANVIL_MIR_OP_UITOFP) {
        bool is_unsigned = instr.op == ANVIL_MIR_OP_UITOFP;
        const char *integer = source;
        if (src_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
            anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "LGR" : "LR", source);
            systemz_emit_integer_normalize(emit, "R0", src_info->size_bits, !is_unsigned);
            integer = "R0";
            /* A zero-extended u8/u16/u32 fits the wider signed domain:
             * convert it at that width. */
            is_unsigned = false;
            systemz_emit_signed_int_to_fp(emit, dst, integer, emit->desc->has_64bit_gprs ? 64 : 32, dst_info->size_bits);
            return;
        }
        if (!is_unsigned) {
            systemz_emit_signed_int_to_fp(emit, dst, integer, src_info->size_bits, dst_info->size_bits);
            return;
        }

        char low_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(low_label, sizeof(low_label), "%s#UIFP_L_%u", emit->func_label, emit->label_counter);
        snprintf(end_label, sizeof(end_label), "%s#UIFP_E_%u", emit->func_label, emit->label_counter++);
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
        anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", ieee ? (dst_info->size_bits == 32 ? "AEBR" : "ADBR") : (dst_info->size_bits == 32 ? "AER" : "ADR"), dst, dst);
        anvil_strbuf_appendf(&emit->code, "         B     %s\n", end_label);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", low_label);
        systemz_emit_signed_int_to_fp(emit, dst, source, src_info->size_bits, dst_info->size_bits);
        anvil_strbuf_appendf(&emit->code, "%-8s DS    0H\n", end_label);
        return;
    }

    bool is_unsigned = instr.op == ANVIL_MIR_OP_FPTOUI;
    if (is_unsigned && emit->desc->has_64bit_gprs && dst_info->size_bits <= 32) {
        /* Every u32 value fits the 64-bit signed conversion. */
        systemz_emit_fp_to_signed_int(emit, dst, source, src_info->size_bits, 64);
        systemz_emit_integer_normalize(emit, dst, dst_info->size_bits, false);
        return;
    }
    if (!is_unsigned || dst_info->size_bits < (emit->desc->has_64bit_gprs ? 64 : 32)) {
        systemz_emit_fp_to_signed_int(emit, dst, source, src_info->size_bits, dst_info->size_bits);
        systemz_emit_integer_normalize(emit, dst, dst_info->size_bits, !is_unsigned);
        return;
    }

    char low_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
    snprintf(low_label, sizeof(low_label), "%s#FPUI_L_%u", emit->func_label, emit->label_counter);
    snprintf(end_label, sizeof(end_label), "%s#FPUI_E_%u", emit->func_label, emit->label_counter++);
    const char *literal;
    if (src_info->size_bits == 32) {
        literal = ieee ? (dst_info->size_bits > 32 ? "=EB'9223372036854775808'" : "=EB'2147483648'") : (dst_info->size_bits > 32 ? "=E'9223372036854775808'" : "=E'2147483648'");
    } else {
        literal = ieee ? (dst_info->size_bits > 32 ? "=DB'9223372036854775808'" : "=DB'2147483648'") : (dst_info->size_bits > 32 ? "=D'9223372036854775808'" : "=D'2147483648'");
    }
    anvil_strbuf_appendf(&emit->code, "         %s    F0,%s\n", src_info->size_bits == 32 ? "LE" : "LD", literal);
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,F0\n", ieee ? (src_info->size_bits == 32 ? "CEBR" : "CDBR") : (src_info->size_bits == 32 ? "CER" : "CDR"), source);
    anvil_strbuf_appendf(&emit->code, "         BL    %s\n", low_label);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,%s\n", ieee ? (src_info->size_bits == 32 ? "SEBR" : "SDBR") : (src_info->size_bits == 32 ? "SER" : "SDR"), source);
    anvil_strbuf_appendf(&emit->code, "         %-5s F0,F0\n", ieee ? (src_info->size_bits == 32 ? "LCEBR" : "LCDBR") : (src_info->size_bits == 32 ? "LCER" : "LCDR"));
    systemz_emit_fp_to_signed_int(emit, dst, "F0", src_info->size_bits, dst_info->size_bits);
    anvil_strbuf_appendf(&emit->code, "         %s    %s,=X'%s'\n", emit->desc->has_64bit_gprs ? "OG" : "O", dst, emit->desc->has_64bit_gprs ? "8000000000000000" : "80000000");
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
        /* Extensions define every bit of the result (TRUNC needs none: a
         * narrow value's high bits are ignored by its users). */
        bool extend = dst_info->reg_class == ANVIL_MIR_REG_GPR && (instr.op == ANVIL_MIR_OP_SEXT || instr.op == ANVIL_MIR_OP_ZEXT) &&
                      src_info->size_bits < dst_info->size_bits;
        if (extend) {
            const char *dst = systemz_vreg_reg_name(emit, instr.def);
            bool sign = instr.op == ANVIL_MIR_OP_SEXT;
            if (src_info->size_bits < 32)
                systemz_emit_integer_normalize(emit, dst, src_info->size_bits, sign);
            else if (emit->desc->has_64bit_gprs)
                anvil_strbuf_appendf(&emit->code, "         %s %s,%s\n", sign ? "LGFR " : "LLGFR", dst, dst);
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
    } else if (anvil_mir_isel_materialize(&emit->isel, instr.def)) {
        systemz_emit_load_imm(emit, systemz_phys_reg(emit, instr.def), instr.imm);
    }
}

static void systemz_emit_symbol_addr(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    char upper[96];
    systemz_symbol_name(upper, sizeof(upper), instr.symbol);
    /* LA reaches only base+4095; an address constant reaches anything
     * (external symbols included). */
    char addend[32] = "";
    if (instr.has_imm && instr.imm != 0)
        snprintf(addend, sizeof(addend), "%+lld", (long long)instr.imm);
    if (emit->desc->has_64bit_gprs)
        anvil_strbuf_appendf(&emit->code, "         LARL  %s,%s%s\n", systemz_vreg_reg_name(emit, instr.def), upper, addend);
    else
        anvil_strbuf_appendf(&emit->code, "         L     %s,=A(%s%s)\n", systemz_vreg_reg_name(emit, instr.def), upper, addend);
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
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits <= 8 && systemz_phys_reg(emit, instr.def) == systemz_phys_reg(emit, addr)) {
        /* IC inserts into a cleared register; when the destination is the
         * address register, clear R1 instead and move the byte over. */
        anvil_strbuf_appendf(&emit->code, "         %s    R1,R1\n         IC    R1,%lld(,%s)\n         %s    %s,R1\n",
                             emit->desc->has_64bit_gprs ? "XGR" : "XR", instr.has_imm ? (long long)instr.imm : 0LL,
                             systemz_vreg_reg_name(emit, addr), emit->desc->has_64bit_gprs ? "LGR" : "LR", systemz_vreg_reg_name(emit, instr.def));
        systemz_emit_narrow_load_suffix(emit, info, instr.def);
        return;
    }
    systemz_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld(,%s)\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def),
                         instr.has_imm ? (long long)instr.imm : 0LL, systemz_vreg_reg_name(emit, addr));
    systemz_emit_narrow_load_suffix(emit, info, instr.def);
}

static void systemz_emit_store(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    anvil_mir_vreg_t value = ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t addr = ANVIL_MIR_NO_VREG;
    systemz_get_uses(emit->mir, index, &value, &addr, NULL);
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, value);
    if (!info)
        return;
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,%lld(,%s)\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, value),
                         instr.has_imm ? (long long)instr.imm : 0LL, systemz_vreg_reg_name(emit, addr));
}

/* Register `reg` = R11 + offset (any offset: LA's displacement is 12 bits). */
static void systemz_emit_frame_address(systemz_emit_t *emit, int reg, int offset)
{
    if (offset >= 0 && offset <= 4095) {
        anvil_strbuf_appendf(&emit->code, "         LA    %s,%d(,R11)\n", systemz_gpr_name(reg), offset);
        return;
    }
    systemz_emit_load_imm(emit, reg, offset);
    anvil_strbuf_appendf(&emit->code, "         %s %s,R11\n", emit->desc->has_64bit_gprs ? "ALGR " : "ALR  ", systemz_gpr_name(reg));
}

/* Frame byte `offset` as a storage operand: a displacement from R11 when it
 * fits, otherwise based on emitter temporary `temp`. */
static const char *systemz_frame_operand(systemz_emit_t *emit, int offset, int temp, char *buf, size_t len)
{
    if (offset >= 0 && offset <= 4095) {
        snprintf(buf, len, "%d(,R11)", offset);
        return buf;
    }
    systemz_emit_frame_address(emit, temp, offset);
    snprintf(buf, len, "0(,%s)", systemz_gpr_name(temp));
    return buf;
}

static void systemz_emit_frame_addr(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    int off = instr.frame_slot >= 0 ? emit->frame_offsets[instr.frame_slot] : 0;
    systemz_emit_frame_address(emit, systemz_phys_reg(emit, instr.def), off);
}

static void systemz_emit_incoming_arg(systemz_emit_t *emit, anvil_mir_instr_info_t instr)
{
    const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
    if (!info || !instr.has_imm)
        return;
    /* The address goes to R14 (R1 keeps the list; base register 0 would
     * mean no base). */
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LG    R14,%lld(,R1)\n", (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         NIHH  R14,X'7FFF'\n");
    } else {
        anvil_strbuf_appendf(&emit->code, "         L     R14,%lld(,R1)\n", (long long)instr.imm);
        anvil_strbuf_append(&emit->code, "         N     R14,=X'7FFFFFFF'\n");
    }
    systemz_emit_narrow_load_prefix(emit, info, instr.def);
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,0(,R14)\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def));
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

    char operand[32];
    const char *value_mem = systemz_frame_operand(emit, value_off, 14, operand, sizeof(operand));
    anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, value), value_mem);
    systemz_emit_frame_address(emit, 0, value_off);
    if (emit->call_arg_is_last && emit->call_arg_is_last[index]) {
        if (emit->desc->has_64bit_gprs) {
            anvil_strbuf_append(&emit->code, "         OIHH  R0,X'8000'\n");
        } else {
            anvil_strbuf_append(&emit->code, "         O     R0,=X'80000000'\n");
        }
    }
    const char *list_mem = systemz_frame_operand(emit, list_off, 14, operand, sizeof(operand));
    anvil_strbuf_appendf(&emit->code, "         %s    R0,%s\n", emit->desc->has_64bit_gprs ? "STG" : "ST", list_mem);
}

static void systemz_emit_call(systemz_emit_t *emit, anvil_mir_instr_info_t instr, size_t index)
{
    (void)index;
    if (emit->outgoing_param_list_offset >= 0 && emit->call_arg_counts && emit->call_arg_counts[index] > 0) {
        systemz_emit_frame_address(emit, 1, emit->outgoing_param_list_offset);
    } else {
        anvil_strbuf_appendf(&emit->code, "         %s    R1,R1\n", emit->desc->has_64bit_gprs ? "XGR" : "XR");
    }
    if (instr.symbol) {
        char upper[96];
        systemz_symbol_name(upper, sizeof(upper), instr.symbol);
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
    /* A word pair: neither result register is allocatable, so the moves
     * cannot clobber each other's source. */
    if (emit->ret_low_word != ANVIL_MIR_NO_VREG)
        anvil_strbuf_appendf(&emit->code, "         LR    R%d,%s\n", emit->desc->abi_ops->pair_low_gpr, systemz_vreg_reg_name(emit, emit->ret_low_word));
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
        if (elem_size != 1 && (emit->features & ANVIL_FEATURE_ZARCH_GIE)) {
            anvil_strbuf_appendf(&emit->code, "         MSGFI R0,%lld\n", elem_size);
        } else if (elem_size != 1) {
            /* No MSGFI before the z10. */
            anvil_strbuf_appendf(&emit->code, "         MSG   R0,=FD'%lld'\n", elem_size);
        }
        anvil_strbuf_appendf(&emit->code,
                             "         LGR   %s,R13\n"
                             "         AGHI  %s,%u\n"
                             "         AGHI  %s,15\n"
                             "         NILL  %s,X'FFF0'\n",
                             dst, dst, local, dst, dst);
        anvil_strbuf_appendf(&emit->code,
                             "         AGR   R0,%s\n"
                             "         LGR   R1,R0\n"
                             "         AGHI  R1,15\n"
                             "         NILL  R1,X'FFF0'\n"
                             "         STG   R11,8(,R1)\n"
                             "         STG   R1,16(,R11)\n"
                             "         LGR   R13,R1\n",
                             dst);
    } else {
        anvil_strbuf_appendf(&emit->code, "         LR    R1,%s\n", src);
        if (elem_size != 1) {
            anvil_strbuf_appendf(&emit->code, "         M     R0,=F'%lld'\n", elem_size);
        }
        anvil_strbuf_append(&emit->code, "         LR    R0,R1\n");
        anvil_strbuf_appendf(&emit->code,
                             "         LR    %s,R13\n"
                             "         LA    %s,%u(,%s)\n"
                             "         LA    %s,15(,%s)\n"
                             "         N     %s,=X'FFFFFFF0'\n",
                             dst, dst, local, dst, dst, dst, dst);
        anvil_strbuf_appendf(&emit->code,
                             "         AR    R0,%s\n"
                             "         LR    R1,R0\n"
                             "         LA    R1,15(,R1)\n"
                             "         N     R1,=X'FFFFFFF0'\n"
                             "         ST    R11,4(,R1)\n"
                             "         ST    R1,8(,R11)\n"
                             "         LR    R13,R1\n",
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
        if (!systemz_emit_binary_imm(emit, instr, index, u0))
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
        systemz_emit_cmp(emit, instr, index, u0, u1);
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
        if (instr.true_block == emit->block + 1)
            break;
        anvil_strbuf_append(&emit->code, "         B     ");
        systemz_emit_branch_target(emit, instr.true_block);
        anvil_strbuf_append(&emit->code, "\n");
        break;
    case ANVIL_MIR_OP_BR_COND: {
        const char *taken = emit->fused_branch;
        emit->fused_branch = NULL;
        if (!taken) {
            const char *cond = systemz_vreg_reg_name(emit, u0);
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", systemz_operand_wide(emit, u0) ? "LTGR" : "LTR", cond, cond);
            taken = "BNE";
        }
        systemz_emit_cond_branch(emit, taken, instr.true_block, instr.false_block);
        break;
    }
    case ANVIL_MIR_OP_RET_VALUE_PART:
        emit->ret_low_word = u0;
        break;
    case ANVIL_MIR_OP_RET:
        systemz_emit_ret(emit, instr, index);
        emit->ret_low_word = ANVIL_MIR_NO_VREG;
        break;
    case ANVIL_MIR_OP_SPILL_LOAD: {
        const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, instr.def);
        int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
        if (info) {
            char operand[32];
            const char *mem = systemz_frame_operand(emit, off, 14, operand, sizeof(operand));
            systemz_emit_narrow_load_prefix(emit, info, instr.def);
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", systemz_load_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, instr.def), mem);
            systemz_emit_narrow_load_suffix(emit, info, instr.def);
        }
        break;
    }
    case ANVIL_MIR_OP_SPILL_STORE: {
        const anvil_mir_vreg_info_t *info = systemz_emit_vreg_info(emit, u0);
        int off = instr.spill_slot >= 0 ? emit->spill_offsets[instr.spill_slot] : 0;
        if (info) {
            char operand[32];
            const char *mem = systemz_frame_operand(emit, off, 14, operand, sizeof(operand));
            anvil_strbuf_appendf(&emit->code, "         %-5s %s,%s\n", systemz_store_op(emit, info->size_bits, info->reg_class), systemz_vreg_reg_name(emit, u0), mem);
        }
        break;
    }
    case ANVIL_MIR_OP_DYN_ALLOCA:
        systemz_emit_dynamic_alloca(emit, instr, u0);
        break;
    case ANVIL_MIR_OP_SELECT: {
        /* Load on condition (z196): test the condition, take `else`, then
         * `then` where it was nonzero (mask 7); a destination already
         * holding `then` takes `else` where it was zero (mask 8). */
        const anvil_mir_vreg_info_t *def_info = systemz_emit_vreg_info(emit, instr.def);
        if (def_info && def_info->reg_class == ANVIL_MIR_REG_GPR && emit->desc->has_64bit_gprs &&
            (emit->features & ANVIL_FEATURE_ZARCH_LOADSTORE)) {
            anvil_strbuf_appendf(&emit->code, "         %s  %s,%s\n", systemz_operand_wide(emit, u0) ? "LTGR" : "LTR ", systemz_vreg_reg_name(emit, u0),
                                 systemz_vreg_reg_name(emit, u0));
            int dst = systemz_phys_reg(emit, instr.def);
            if (dst == systemz_phys_reg(emit, u1)) {
                anvil_strbuf_appendf(&emit->code, "         LOCGR %s,%s,8\n", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, u2));
                break;
            }
            if (dst != systemz_phys_reg(emit, u2))
                systemz_emit_copy(emit, instr.def, u2);
            anvil_strbuf_appendf(&emit->code, "         LOCGR %s,%s,7\n", systemz_vreg_reg_name(emit, instr.def), systemz_vreg_reg_name(emit, u1));
            break;
        }
        char true_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        char end_label[SYSTEMZ_INTERNAL_LABEL_CAP];
        snprintf(true_label, sizeof(true_label), "%s#SEL_T_%u", emit->func_label, emit->label_counter);
        snprintf(end_label, sizeof(end_label), "%s#SEL_E_%u", emit->func_label, emit->label_counter++);
        /* Test the condition at its width (no copy needed: LTR leaves it). */
        anvil_strbuf_appendf(&emit->code, "         %s  %s,%s\n", systemz_operand_wide(emit, u0) ? "LTGR" : "LTR ", systemz_vreg_reg_name(emit, u0),
                             systemz_vreg_reg_name(emit, u0));
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
        /* The low word of a pair result, right after its call. */
        anvil_strbuf_appendf(&emit->code, "         LR    %s,R%d\n", systemz_vreg_reg_name(emit, instr.def), emit->desc->abi_ops->pair_low_gpr);
        break;
    case ANVIL_MIR_OP_INVALID:
    case ANVIL_MIR_OP_VA_START:
    case ANVIL_MIR_OP_ATOMIC:
    case ANVIL_MIR_OP_VECTOR:
    case ANVIL_MIR_OP_SCALAR:
        emit->code.failed = true;
        break;
    }
}

/* R12 is the literal base. With relative branches (IMMREL) nothing else
 * is base-addressed, so R12 points at the pool itself and the function may
 * be as long as relative branches reach; without them code and literals
 * share one 4 KB base at the entry point. */
void systemz_emit_establish_base(systemz_emit_t *emit)
{
    const char *label = emit->func_label;
    if (!(emit->features & ANVIL_FEATURE_ZARCH_IMMREL)) {
        anvil_strbuf_append(&emit->code, emit->desc->has_64bit_gprs ? "         LGR   R12,R15\n" : "         LR    R12,R15\n");
        anvil_strbuf_appendf(&emit->code, "         USING %s,R12\n", label);
        return;
    }
    if (emit->desc->has_64bit_gprs) {
        anvil_strbuf_appendf(&emit->code, "         LARL  R12,%s#LIT\n", label);
    } else {
        /* BRAS leaves the address of the (unaligned) constant in R12. */
        anvil_strbuf_appendf(&emit->code, "         BRAS  R12,%s#LB\n         DC    AL4(%s#LIT)\n", label, label);
        anvil_strbuf_appendf(&emit->code, "%s#LB L     R12,0(,R12)\n", label);
    }
    anvil_strbuf_appendf(&emit->code, "         USING %s#LIT,R12\n", label);
}

void systemz_emit_literal_pool(systemz_emit_t *emit)
{
    if (emit->features & ANVIL_FEATURE_ZARCH_IMMREL)
        anvil_strbuf_appendf(&emit->code, "%s#LIT LTORG\n", emit->func_label);
    else
        anvil_strbuf_append(&emit->code, "         LTORG\n");
}

static void systemz_emit_string_literals(systemz_emit_t *emit)
{
    for (size_t i = 0; i < anvil_mir_num_string_literals(emit->mir); i++) {
        anvil_mir_string_literal_info_t info;
        if (!anvil_mir_get_string_literal_info(emit->mir, i, &info))
            continue;
        char upper[96];
        systemz_symbol_name(upper, sizeof(upper), info.label);
        /* LARL reaches only halfword boundaries. */
        anvil_strbuf_append(&emit->code, "         DS    0H\n");
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

/* With the relative-and-immediate instructions (S/390 G1+), branches to
 * labels become relative (J, Jcc, BRC: +-64 KB), independent of the base
 * register's 4 KB reach.  Rewrites only whole-line based branches whose
 * operand is a label; register branches (BR, BCR, BALR) are untouched. */
static void systemz_use_relative_branches(anvil_strbuf_t *code)
{
    static const char *const based[] = {"B", "BE", "BNE", "BL", "BNL", "BH", "BNH", "BM", "BNM", "BZ", "BNZ", "BO", "BNO", "BP", "BNP"};
    anvil_strbuf_t out;
    anvil_strbuf_init(&out);
    const char *cursor = code->data ? code->data : "";
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        char mnemonic[8] = "";
        const char *operand = NULL;
        if (length > 16 && strncmp(cursor, "         ", 9) == 0) {
            size_t m = 0;
            while (m < 6 && cursor[9 + m] != ' ' && cursor[9 + m] != '\n')
                m++;
            memcpy(mnemonic, cursor + 9, m);
            mnemonic[m] = '\0';
            operand = cursor + 15;
        }
        bool rewritten = false;
        if (operand && !memchr(operand, '(', length - 15)) {
            if (strcmp(mnemonic, "BC") == 0) {
                anvil_strbuf_appendf(&out, "         BRC   %.*s", (int)(length - 15), operand);
                rewritten = true;
            } else {
                for (size_t i = 0; i < sizeof(based) / sizeof(based[0]); i++) {
                    if (strcmp(mnemonic, based[i]) == 0) {
                        anvil_strbuf_appendf(&out, "         J%-4s %.*s", based[i] + 1, (int)(length - 15), operand);
                        rewritten = true;
                        break;
                    }
                }
            }
        }
        if (!rewritten)
            anvil_strbuf_appendf(&out, "%.*s", (int)length, cursor);
        cursor += length;
    }
    if (out.failed) {
        code->failed = true;
        anvil_strbuf_destroy(&out);
        return;
    }
    anvil_strbuf_destroy(code);
    *code = out;
}

/* What one base register addresses, less a margin for the estimate. */
#define SYSTEMZ_BASE_REACH 3800

/* ------------------------------------------------------------------
 * Functions longer than one base register reaches (no relative branches).
 * ------------------------------------------------------------------ */

/* Bytes an HLASM statement assembles to: instruction length from its
 * format (RR 2, SS 6, the rest 4), DC data by its operand, literals in the
 * pool counted once each. A rough upper bound is enough. */
static size_t systemz_statement_bytes(const char *line, size_t length)
{
    static const char *const rr[] = {"LR", "AR", "SR", "MR", "DR", "NR", "OR", "XR", "CR", "CLR", "LTR", "LCR", "LPR", "LNR", "BR", "BCR",
                                     "BALR", "BASR", "ALR", "SLR", "LDR", "LER", "ADR", "AER", "SDR", "SER", "MDR", "MER", "DDR", "DER",
                                     "CDR", "CER", "LTDR", "LTER", "LCDR", "LCER", "LPDR", "LPER", "LNDR", "LNER", "HDR", "HER", "SPM", "BCTR"};
    static const char *const ss[] = {"MVC", "CLC", "NC", "OC", "XC", "MVZ", "MVN", "MVO", "PACK", "UNPK", "ZAP", "AP", "SP", "MP", "DP",
                                     "CP", "ED", "EDMK", "TR", "TRT", "SRP", "MVCK", "MVCP", "MVCS"};
    if (length < 10 || line[0] == '*' || line[9] == ' ')
        return 0;
    char op[8] = "";
    size_t n = 0;
    while (n < 7 && 9 + n < length && line[9 + n] != ' ' && line[9 + n] != '\n')
        op[n] = line[9 + n], n++;
    op[n] = '\0';
    if (!strcmp(op, "DS") || !strcmp(op, "EQU") || !strcmp(op, "USING") || !strcmp(op, "DROP") || !strcmp(op, "LTORG") || !strcmp(op, "ENTRY") ||
        !strcmp(op, "EXTRN") || !strcmp(op, "CSECT") || !strcmp(op, "AMODE") || !strcmp(op, "RMODE") || !strcmp(op, "END"))
        return 2;   /* alignment at most */
    if (!strcmp(op, "DC")) {
        const char *quote = memchr(line, '\'', length);
        const char *end = quote ? memchr(quote + 1, '\'', length - (size_t)(quote + 1 - line)) : NULL;
        return quote && end && quote[-1] == 'X' ? (size_t)(end - quote) / 2 + 1 : 8;
    }
    for (size_t i = 0; i < sizeof(rr) / sizeof(rr[0]); i++)
        if (!strcmp(op, rr[i]))
            return 2;
    for (size_t i = 0; i < sizeof(ss) / sizeof(ss[0]); i++)
        if (!strcmp(op, ss[i]))
            return 6;
    return 4;
}

static int systemz_compare_literals(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Bytes of one literal (=F'..', =A(..), =XL8'..', =X'..', ...). */
static size_t systemz_literal_bytes(const char *literal)
{
    const char *type = literal + 1;
    if (!strncmp(type, "XL8", 3) || !strncmp(type, "D'", 2) || !strncmp(type, "AD(", 3) || !strncmp(type, "FD'", 3))
        return 8;
    if (!strncmp(type, "H'", 2))
        return 2;
    if (type[0] == 'X' && type[1] == '\'') {
        const char *end = strchr(type + 2, '\'');
        return end ? (size_t)(end - type - 2 + 1) / 2 : 8;
    }
    return 4;
}

/* Code bytes and literal-pool bytes (distinct literals, each the last
 * operand of its statement) of one function's text. */
static bool systemz_function_bytes(const anvil_strbuf_t *code, size_t *code_bytes, size_t *pool_bytes)
{
    const char *cursor = code->data ? code->data : "";
    size_t count = 0, capacity = 64;
    char **literals = malloc(capacity * sizeof(*literals));
    bool ok = literals != NULL;
    *code_bytes = 0;
    *pool_bytes = 0;
    while (ok && *cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        *code_bytes += systemz_statement_bytes(cursor, length + 1);
        const char *literal = length > 9 && cursor[0] != '*' ? memchr(cursor, '=', length) : NULL;
        if (literal) {
            if (count == capacity) {
                char **grown = realloc(literals, 2 * capacity * sizeof(*literals));
                ok = grown != NULL;
                literals = ok ? grown : literals;
                capacity *= 2;
            }
            if (ok) {
                size_t literal_length = length - (size_t)(literal - cursor);
                literals[count] = malloc(literal_length + 1);
                ok = literals[count] != NULL;
                if (ok) {
                    memcpy(literals[count], literal, literal_length);
                    literals[count++][literal_length] = '\0';
                }
            }
        }
        cursor += length + (end ? 1 : 0);
    }
    if (ok) {
        qsort(literals, count, sizeof(*literals), systemz_compare_literals);
        for (size_t i = 0; i < count; i++)
            if (i == 0 || strcmp(literals[i], literals[i - 1]))
                *pool_bytes += systemz_literal_bytes(literals[i]);
    }
    for (size_t i = 0; literals && i < count; i++)
        free(literals[i]);
    free(literals);
    return ok;
}

/* R12 addresses the literal pool instead of the entry point (loaded at
 * entry: BAL over an address constant, R15 being the entry address), and
 * every based branch loads its target from the pool and branches through
 * R15, which holds no value at a branch. */
static void systemz_use_far_branches(anvil_strbuf_t *code, const char *label)
{
    static const struct { const char *name; unsigned mask; } based[] = {
        {"B", 15}, {"BE", 8}, {"BZ", 8}, {"BNE", 7}, {"BNZ", 7}, {"BL", 4}, {"BM", 4}, {"BNL", 11}, {"BNM", 11}, {"BH", 2}, {"BP", 2},
        {"BNH", 13}, {"BNP", 13}, {"BO", 1}, {"BNO", 14},
    };
    char entry_base[128], pool[160];
    snprintf(entry_base, sizeof(entry_base), "         USING %s,R12\n", label);
    snprintf(pool, sizeof(pool), "         LTORG\n");
    anvil_strbuf_t out;
    anvil_strbuf_init(&out);
    const char *cursor = code->data ? code->data : "";
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        bool rewritten = false;
        if (length == strlen(entry_base) && !strncmp(cursor, entry_base, length)) {
            /* Replaces "LR R12,R15" (already copied) + USING: drop the LR. */
            size_t lr = strlen("         LR    R12,R15\n");
            if (out.len >= lr && !strncmp(out.data + out.len - lr, "         LR    R12,R15\n", lr))
                out.len -= lr;
            anvil_strbuf_appendf(&out, "         BAL   R12,12(,R15)\n         DC    AL4(%s#LIT)\n         L     R12,0(,R12)\n         USING %s#LIT,R12\n",
                                 label, label);
            rewritten = true;
        } else if (length == strlen(pool) && !strncmp(cursor, pool, length)) {
            anvil_strbuf_appendf(&out, "%s#LIT LTORG\n", label);
            rewritten = true;
        } else if (length > 16 && !strncmp(cursor, "         ", 9)) {
            char op[8] = "";
            size_t n = 0;
            while (n < 6 && cursor[9 + n] != ' ' && cursor[9 + n] != '\n')
                op[n] = cursor[9 + n], n++;
            op[n] = '\0';
            const char *operand = cursor + 15;
            size_t operand_length = length - 16;
            if (!memchr(operand, '(', operand_length)) {
                if (!strcmp(op, "BC")) {
                    const char *comma = memchr(operand, ',', operand_length);
                    if (comma) {
                        anvil_strbuf_appendf(&out, "         L     R15,=A(%.*s)\n         BCR   %.*s,R15\n", (int)(operand_length - (size_t)(comma + 1 - operand)),
                                             comma + 1, (int)(comma - operand), operand);
                        rewritten = true;
                    }
                }
                for (size_t i = 0; !rewritten && i < sizeof(based) / sizeof(based[0]); i++) {
                    if (strcmp(op, based[i].name))
                        continue;
                    anvil_strbuf_appendf(&out, "         L     R15,=A(%.*s)\n         BCR   %u,R15\n", (int)operand_length, operand, based[i].mask);
                    rewritten = true;
                }
            }
        }
        if (!rewritten)
            anvil_strbuf_appendf(&out, "%.*s", (int)length, cursor);
        cursor += length;
    }
    if (out.failed) {
        code->failed = true;
        anvil_strbuf_destroy(&out);
        return;
    }
    anvil_strbuf_destroy(code);
    *code = out;
}

bool systemz_emit_mir_ex(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, anvil_fp_format_t fp_format,
                         anvil_cpu_features_t features, char **output, size_t *len)
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
    emit.ret_low_word = ANVIL_MIR_NO_VREG;
    emit.mir = mir;
    emit.fp_format = fp_format;
    emit.features = features;
    anvil_strbuf_init(&emit.code);
    const char *mir_name = anvil_mir_func_name(mir);
    if (mir_name && strlen(mir_name) >= sizeof(emit.func_label)) {
        anvil_strbuf_destroy(&emit.code);
        return false;
    }
    systemz_symbol_name(emit.func_label, sizeof(emit.func_label), mir_name);

    bool ok = systemz_prepare_frame(&emit) && anvil_mir_isel_facts_build(&emit.isel, mir, systemz_takes_immediate, &emit);
    if (ok) {
        systemz_emit_header(&emit);
        desc->abi_ops->emit_prologue(&emit);
        for (size_t b = 0; b < anvil_mir_num_blocks(mir) && !emit.code.failed; b++) {
            if (b != 0) {
                char label[SYSTEMZ_INTERNAL_LABEL_CAP];
                systemz_block_label(&emit, (anvil_mir_block_t)b, label, sizeof(label));
                anvil_strbuf_appendf(&emit.code, "%-8s DS    0H\n", label);
            }
            emit.block = (anvil_mir_block_t)b;
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
            systemz_emit_literal_pool(&emit);
            systemz_emit_string_literals(&emit);
            anvil_strbuf_appendf(&emit.code, "%s#DYN EQU   %d\n", emit.func_label, emit.frame_size);
            if (features & ANVIL_FEATURE_ZARCH_IMMREL)
                systemz_use_relative_branches(&emit.code);
            else {
                /* Too long for one base: R12 then addresses only the pool,
                 * which must itself stay within its reach. */
                size_t code_bytes = 0, pool_bytes = 0;
                if (!systemz_function_bytes(&emit.code, &code_bytes, &pool_bytes))
                    ok = false;
                else if (code_bytes + pool_bytes > SYSTEMZ_BASE_REACH)
                    systemz_use_far_branches(&emit.code, emit.func_label);
                /* Branch targets join the pool as address constants. */
                if (ok && code_bytes + pool_bytes > SYSTEMZ_BASE_REACH && !systemz_function_bytes(&emit.code, &code_bytes, &pool_bytes))
                    ok = false;
                if (ok && pool_bytes > 4096 - 8)
                    ok = false;   /* literal pool beyond one base register */
            }
        }
    }

    anvil_mir_isel_facts_destroy(&emit.isel);
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
