#include "ppc_internal.h"

static bool ppc_reg_is_in_set(int reg, const int *regs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (regs[i] == reg)
            return true;
    }
    return false;
}

static bool ppc_legal_size_for_class(const anvil_ppc_target_desc_t *desc, const anvil_mir_vreg_info_t *info)
{
    if (!desc || !info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        if (info->size_bits != 8 && info->size_bits != 16 && info->size_bits != 32 && info->size_bits != 64) {
            return false;
        }
        return info->size_bits <= desc->word_size * 8;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR)
        return info->size_bits == 32 || info->size_bits == 64;
    return info->reg_class == ANVIL_MIR_REG_VECTOR && info->size_bits == 128;
}

static bool ppc_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg)
        return true;
    if (info->fixed_phys_reg < 0)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR)
        return info->fixed_phys_reg < 32;
    if (info->reg_class == ANVIL_MIR_REG_FPR || info->reg_class == ANVIL_MIR_REG_VECTOR)
        return info->fixed_phys_reg < 32;
    return false;
}

static const anvil_mir_vreg_info_t *ppc_legal_vreg_info(const anvil_ppc_target_desc_t *desc, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses invalid vreg", instr_index);
        return NULL;
    }
    if (!ppc_legal_size_for_class(desc, info)) {
        anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses unsupported vreg class/size", instr_index);
        return NULL;
    }
    if (!ppc_legal_fixed_reg(info)) {
        anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses invalid fixed register", instr_index);
        return NULL;
    }
    return info;
}

static bool ppc_legal_pointer_operand(const anvil_ppc_target_desc_t *desc, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = ppc_legal_vreg_info(desc, mir, vreg, instr_index, error, error_len);
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == desc->word_size * 8) {
        return true;
    }
    return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu requires a pointer-sized operand", instr_index);
}

static bool ppc_legal_same_class_and_size(const anvil_mir_vreg_info_t *a, const anvil_mir_vreg_info_t *b)
{
    return a && b && a->reg_class == b->reg_class && a->size_bits == b->size_bits;
}

static bool ppc_legal_numeric_conversion(size_t instr_index, const anvil_mir_instr_info_t *instr, const anvil_mir_vreg_info_t *dst, const anvil_mir_vreg_info_t *src, char *error, size_t error_len)
{
    bool legal = false;
    switch (instr->op) {
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
        legal = src->reg_class == ANVIL_MIR_REG_GPR && dst->reg_class == ANVIL_MIR_REG_FPR;
        break;
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
        legal = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_GPR;
        break;
    case ANVIL_MIR_OP_FPEXT:
        legal = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_FPR && src->size_bits == 32 && dst->size_bits == 64;
        break;
    case ANVIL_MIR_OP_FPTRUNC:
        legal = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_FPR && src->size_bits == 64 && dst->size_bits == 32;
        break;
    default:
        break;
    }
    if (legal)
        return true;
    return anvil_format_failure(error, error_len, "PowerPC MIR conversion %zu has incompatible source/destination types", instr_index);
}

static bool ppc_legal_binary(const anvil_ppc_target_desc_t *desc, const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def = ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info = ppc_legal_vreg_info(desc, mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info = ppc_legal_vreg_info(desc, mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info)
        return false;

    if (!ppc_legal_same_class_and_size(def, lhs_info) || !ppc_legal_same_class_and_size(lhs_info, rhs_info)) {
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu has incompatible binary operands", instr_index);
    }

    bool fp_op = def->reg_class == ANVIL_MIR_REG_FPR;
    switch (instr->op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
        return true;
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_FDIV:
        return fp_op || def->reg_class == ANVIL_MIR_REG_GPR;
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
        return def->reg_class == ANVIL_MIR_REG_GPR;
    default:
        break;
    }

    return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses an illegal binary opcode/class pair", instr_index);
}

static bool ppc_legal_call(const anvil_ppc_target_desc_t *desc, const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->call_cc != ANVIL_CC_SYSV) {
        return anvil_format_failure(error, error_len, "PowerPC MIR call %zu uses an unsupported calling convention", instr_index);
    }
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def = ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len);
        if (!def)
            return false;
        int ret_reg = def->reg_class == ANVIL_MIR_REG_FPR ? desc->fpr_return_reg : desc->gpr_return_reg;
        if (!def->has_fixed_reg || def->fixed_phys_reg != ret_reg) {
            return anvil_format_failure(error, error_len, "PowerPC MIR call %zu result must be fixed to ABI result register", instr_index);
        }
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return anvil_format_failure(error, error_len, "PowerPC MIR indirect call %zu requires a target register", instr_index);
        }

        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info = ppc_legal_vreg_info(desc, mir, target, instr_index, error, error_len);
        if (!target_info)
            return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR || target_info->size_bits != desc->word_size * 8 || !target_info->has_fixed_reg || target_info->fixed_phys_reg != desc->indirect_call_reg) {
            return anvil_format_failure(error, error_len, "PowerPC MIR indirect call %zu target must be fixed to ABI linkage register", instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        const anvil_mir_vreg_info_t *info = ppc_legal_vreg_info(desc, mir, use, instr_index, error, error_len);
        if (!info)
            return false;
        if (!info->has_fixed_reg) {
            return anvil_format_failure(error, error_len, "PowerPC MIR call %zu argument %zu must use a fixed ABI register", instr_index, u - arg_start);
        }
        if (info->reg_class == ANVIL_MIR_REG_GPR && ppc_reg_is_in_set(info->fixed_phys_reg, desc->gpr_arg_regs, desc->num_gpr_arg_regs)) {
            continue;
        }
        if (info->reg_class == ANVIL_MIR_REG_FPR && ppc_reg_is_in_set(info->fixed_phys_reg, desc->fpr_arg_regs, desc->num_fpr_arg_regs)) {
            continue;
        }
        return anvil_format_failure(error, error_len, "PowerPC MIR call %zu argument %zu has an invalid fixed ABI register", instr_index, u - arg_start);
    }

    return true;
}

static bool ppc_legal_instr(const anvil_ppc_target_desc_t *desc, const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG && !ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!ppc_legal_vreg_info(desc, mir, use, instr_index, error, error_len)) {
            return false;
        }
    }

    switch (instr->op) {
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
        return ppc_legal_binary(desc, mir, instr_index, instr, error, error_len);

    case ANVIL_MIR_OP_VECTOR:
    case ANVIL_MIR_OP_SCALAR:
        if (anvil_vector_isa_for_mir(ppc_vector_isa, ppc_vector_isa_count(desc), mir, instr_index, instr, anvil_mir_func_cpu_features(mir)))
            return true;
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu has no form on this CPU", instr_index);
    case ANVIL_MIR_OP_ATOMIC:
        return instr->ir_op == ANVIL_OP_ATOMIC_FENCE ||
               ppc_legal_pointer_operand(desc, mir, anvil_mir_get_instr_use(mir, instr_index, 0), instr_index, error, error_len);
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_STORE: {
        size_t base = instr->op == ANVIL_MIR_OP_LOAD ? 0 : 1;
        if (instr->index_scale && (instr->index_scale != 1 || (instr->has_imm && instr->imm != 0)))
            return anvil_format_failure(error, error_len, "PowerPC MIR access %zu: indexed forms take neither scale nor displacement", instr_index);
        return ppc_legal_pointer_operand(desc, mir, anvil_mir_get_instr_use(mir, instr_index, base), instr_index, error, error_len) &&
               (!instr->index_scale ||
                ppc_legal_pointer_operand(desc, mir, anvil_mir_get_instr_use(mir, instr_index, base + 1), instr_index, error, error_len));
    }
    case ANVIL_MIR_OP_SYMBOL_ADDR:
    case ANVIL_MIR_OP_FRAME_ADDR:
    case ANVIL_MIR_OP_VA_START:
    case ANVIL_MIR_OP_DYN_ALLOCA: {
        const anvil_mir_vreg_info_t *def = ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len);
        if (def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == desc->word_size * 8) {
            return true;
        }
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu must define a pointer-sized GPR", instr_index);
    }
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        /* Word aligned; a 64-bit ABI single float sits in the low word of
         * its doubleword. */
        if (instr->has_imm && instr->imm >= 0 && (instr->imm % 4) == 0) {
            return true;
        }
        return anvil_format_failure(error, error_len, "PowerPC MIR stack instruction %zu needs a word-aligned stack offset", instr_index);
    case ANVIL_MIR_OP_CALL:
        return ppc_legal_call(desc, mir, instr_index, instr, error, error_len);
    case ANVIL_MIR_OP_SELECT: {
        const anvil_mir_vreg_info_t *def = ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len);
        const anvil_mir_vreg_info_t *then_info = ppc_legal_vreg_info(desc, mir, anvil_mir_get_instr_use(mir, instr_index, 1), instr_index, error, error_len);
        const anvil_mir_vreg_info_t *else_info = ppc_legal_vreg_info(desc, mir, anvil_mir_get_instr_use(mir, instr_index, 2), instr_index, error, error_len);
        if (ppc_legal_same_class_and_size(def, then_info) && ppc_legal_same_class_and_size(then_info, else_info)) {
            return true;
        }
        return anvil_format_failure(error, error_len, "PowerPC MIR select %zu has incompatible value operands", instr_index);
    }
    case ANVIL_MIR_OP_SPILL_LOAD:
    case ANVIL_MIR_OP_SPILL_STORE:
    case ANVIL_MIR_OP_MOV:
    case ANVIL_MIR_OP_COPY:
    case ANVIL_MIR_OP_NEG:
    case ANVIL_MIR_OP_NOT:
    case ANVIL_MIR_OP_FABS:
    case ANVIL_MIR_OP_ZEXT:
    case ANVIL_MIR_OP_SEXT:
    case ANVIL_MIR_OP_TRUNC:
    case ANVIL_MIR_OP_BITCAST:
        return true;
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC: {
        if (instr->num_uses != 1) {
            return anvil_format_failure(error, error_len, "PowerPC MIR conversion %zu needs one source", instr_index);
        }
        const anvil_mir_vreg_info_t *dst = ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len);
        const anvil_mir_vreg_info_t *src = ppc_legal_vreg_info(desc, mir, anvil_mir_get_instr_use(mir, instr_index, 0), instr_index, error, error_len);
        return dst && src && ppc_legal_numeric_conversion(instr_index, instr, dst, src, error, error_len);
    }
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
    case ANVIL_MIR_OP_RET:
    case ANVIL_MIR_OP_BR:
    case ANVIL_MIR_OP_BR_COND:
        return true;
    case ANVIL_MIR_OP_CALL_RESULT: {
        const anvil_mir_vreg_info_t *def = instr->def != ANVIL_MIR_NO_VREG ? ppc_legal_vreg_info(desc, mir, instr->def, instr_index, error, error_len) : NULL;
        /* Second result register: r4 (64-bit pair or two-doubleword
         * aggregate) or f2-f8 (floating aggregate). */
        bool gpr = def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == desc->word_size * 8 && def->fixed_phys_reg == desc->gpr_return_reg + 1;
        bool fpr = def && def->reg_class == ANVIL_MIR_REG_FPR && def->fixed_phys_reg > desc->fpr_return_reg && def->fixed_phys_reg <= desc->fpr_return_reg + 7;
        if (!instr->symbol && instr->num_uses == 0 && def && def->has_fixed_reg && (gpr || fpr)) {
            return true;
        }
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu has an invalid call-result pseudo", instr_index);
    }
    case ANVIL_MIR_OP_KEEPALIVE:
        return true;
    case ANVIL_MIR_OP_RET_VALUE_PART:
        return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses an unsupported pseudo", instr_index);
    case ANVIL_MIR_OP_INVALID:
    default:
        break;
    }

    return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu uses unsupported opcode", instr_index);
}

bool anvil_ppc_verify_mir_legal(const anvil_mir_func_t *mir, anvil_ppc_variant_t variant, char *error, size_t error_len)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (error && error_len > 0)
        error[0] = '\0';
    if (!desc) {
        return anvil_format_failure(error, error_len, "unknown PowerPC variant");
    }
    if (!anvil_mir_verify(mir, error, error_len))
        return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return anvil_format_failure(error, error_len, "PowerPC MIR instruction %zu is not inspectable", i);
        }
        if (!ppc_legal_instr(desc, mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

/* Calls, and conversions the printer turns into helper calls, end the
 * values in volatile registers; atomics declared their own temporaries. */
static bool ppc_add_call_clobbers(anvil_mir_func_t *mir, const anvil_ppc_target_desc_t *desc, anvil_cpu_features_t features)
{
    const ppc_abi_ops_t *abi = desc->abi_ops;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;
        if (info.op != ANVIL_MIR_OP_CALL && !ppc_conversion_calls_helper(mir, index, &info, features))
            continue;
        if (!anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | abi->call_gpr_clobbers) ||
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | abi->call_fpr_clobbers) ||
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_VECTOR, info.clobbers[ANVIL_MIR_REG_VECTOR] | abi->call_vector_clobbers))
            return false;
    }
    return true;
}

/* X-form accesses: base + index, unscaled, no displacement. */
static bool ppc_address_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits)
{
    (void)user;
    (void)access_bits;
    return scale == 1 && disp == 0;
}

bool anvil_ppc_regalloc_mir(anvil_mir_func_t *mir, anvil_ppc_variant_t variant)
{
    /* Without a CPU model every conversion may call a helper. */
    return anvil_ppc_regalloc_mir_features(mir, variant, 0);
}

bool anvil_ppc_regalloc_mir_features(anvil_mir_func_t *mir, anvil_ppc_variant_t variant, anvil_cpu_features_t features)
{
    const anvil_ppc_target_desc_t *desc = anvil_ppc_get_target_desc(variant);
    if (!desc || !mir || !ppc_add_call_clobbers(mir, desc, features))
        return false;

    /* f14-f31 are nonvolatile only as FPRs: the VSX upper halves are not.
     * Vectors live in the AltiVec registers (vs32-vs63), volatile ones only;
     * v0 and v1 are the printer's temporaries. */
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs,
         .partially_preserved = ~UINT64_C(0) << 14 & UINT64_C(0xFFFFFFFF), .preserved_bits = 64},
        {.reg_class = ANVIL_MIR_REG_VECTOR, .num_phys_regs = PPC_NUM_ALLOC_VECTOR_REGS, .phys_regs = ppc_alloc_vector_regs},
    };
    anvil_regalloc_class_config_t scratch_configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs},
        {.reg_class = ANVIL_MIR_REG_VECTOR, .num_phys_regs = PPC_NUM_SCRATCH_VECTOR_REGS, .phys_regs = ppc_scratch_vector_regs},
    };

    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0) || !anvil_mir_duplicate_branch_targets(mir, 4) || !anvil_mir_local_cse(mir) || !anvil_mir_fold_address_arithmetic(mir, ppc_address_mode, NULL))
        return false;
    if (!anvil_mir_order_blocks(mir) || !anvil_mir_coalesce_copies(mir))
        return false;
    if (!anvil_ppc_verify_mir_legal(mir, variant, NULL, 0))
        return false;
    if (!anvil_regalloc_linear_scan_classes(mir, configs, sizeof(configs) / sizeof(configs[0]))) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs, sizeof(scratch_configs) / sizeof(scratch_configs[0]))) {
        return false;
    }
    return anvil_ppc_verify_mir_legal(mir, variant, NULL, 0);
}
