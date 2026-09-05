#include "systemz_internal.h"

static bool systemz_legal_fail(char *error, size_t error_len, const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static bool systemz_size_legal(const anvil_mainframe_target_desc_t *desc, const anvil_mir_vreg_info_t *info)
{
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    if (info->reg_class != ANVIL_MIR_REG_GPR)
        return false;
    if (info->size_bits == 8 || info->size_bits == 16 || info->size_bits == 32) {
        return true;
    }
    return desc->has_64bit_gprs && info->size_bits == 64;
}

static const anvil_mir_vreg_info_t *systemz_vreg_info(const anvil_mainframe_target_desc_t *desc, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        systemz_legal_fail(error, error_len, "invalid vreg %u", vreg);
        return NULL;
    }
    if (!systemz_size_legal(desc, info)) {
        systemz_legal_fail(error, error_len, "illegal vreg %u size %u for %s", vreg, info->size_bits, desc->name);
        return NULL;
    }
    return info;
}

bool systemz_is_call_preparation(const anvil_mir_instr_info_t *instr)
{
    return instr->op == ANVIL_MIR_OP_SPILL_LOAD || (instr->op == ANVIL_MIR_OP_MOV && instr->has_imm && instr->num_uses == 0);
}

static bool systemz_verify_call_bundles(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    size_t num_instrs = anvil_mir_num_instrs(mir);
    for (size_t i = 0; i < num_instrs; i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return systemz_legal_fail(error, error_len, "cannot inspect instruction %zu", i);
        }
        if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG)
            continue;

        size_t expected = 0;
        anvil_mir_block_t block = instr.block;
        for (;;) {
            if (!instr.has_imm || instr.imm != (int64_t)expected || instr.num_uses != 1 || instr.block != block) {
                return systemz_legal_fail(error, error_len, "malformed call argument bundle at instruction %zu", i);
            }
            expected++;
            i++;
            /* Reloads and rematerialized constants may prepare the next
               argument inside the logical call bundle. */
            while (i < num_instrs && anvil_mir_get_instr_info(mir, i, &instr) && instr.block == block && systemz_is_call_preparation(&instr)) {
                i++;
            }
            if (i >= num_instrs || !anvil_mir_get_instr_info(mir, i, &instr)) {
                return systemz_legal_fail(error, error_len, "unterminated call argument bundle");
            }
            if (instr.op != ANVIL_MIR_OP_CALL_STACK_ARG || instr.block != block) {
                break;
            }
        }

        if (instr.op != ANVIL_MIR_OP_CALL || instr.block != block) {
            return systemz_legal_fail(error, error_len, "call argument bundle is not followed by its call");
        }
    }
    return true;
}

static bool systemz_verify_numeric_cast(const anvil_mir_func_t *mir, size_t index, anvil_mir_instr_info_t instr, char *error, size_t error_len)
{
    if (instr.num_uses != 1 || instr.def == ANVIL_MIR_NO_VREG) {
        return systemz_legal_fail(error, error_len, "numeric conversion must have one source and one result");
    }
    const anvil_mir_vreg_info_t *dst = anvil_mir_get_vreg_info(mir, instr.def);
    anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(mir, index, 0);
    const anvil_mir_vreg_info_t *src = anvil_mir_get_vreg_info(mir, src_vreg);
    if (!src || !dst)
        return systemz_legal_fail(error, error_len, "invalid conversion vreg");

    bool valid = false;
    switch (instr.op) {
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
        valid = src->reg_class == ANVIL_MIR_REG_GPR && dst->reg_class == ANVIL_MIR_REG_FPR && (src->size_bits == 8 || src->size_bits == 16 || src->size_bits == 32 || src->size_bits == 64) &&
                (dst->size_bits == 32 || dst->size_bits == 64);
        break;
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
        valid = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_GPR && (src->size_bits == 32 || src->size_bits == 64) &&
                (dst->size_bits == 8 || dst->size_bits == 16 || dst->size_bits == 32 || dst->size_bits == 64);
        break;
    case ANVIL_MIR_OP_FPEXT:
        valid = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_FPR && src->size_bits == 32 && dst->size_bits == 64;
        break;
    case ANVIL_MIR_OP_FPTRUNC:
        valid = src->reg_class == ANVIL_MIR_REG_FPR && dst->reg_class == ANVIL_MIR_REG_FPR && src->size_bits == 64 && dst->size_bits == 32;
        break;
    default:
        return true;
    }
    return valid || systemz_legal_fail(error, error_len, "invalid numeric conversion register classes or widths");
}

bool anvil_mainframe_verify_mir_legal(const anvil_mir_func_t *mir, anvil_mainframe_variant_t variant, char *error, size_t error_len)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir) {
        return systemz_legal_fail(error, error_len, "invalid mainframe MIR input");
    }
    if (!anvil_mir_verify(mir, error, error_len))
        return false;
    if (!systemz_verify_call_bundles(mir, error, error_len))
        return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return systemz_legal_fail(error, error_len, "cannot inspect instruction %zu", i);
        }
        if (instr.def != ANVIL_MIR_NO_VREG && !systemz_vreg_info(desc, mir, instr.def, error, error_len)) {
            return false;
        }
        for (size_t u = 0; u < instr.num_uses; u++) {
            anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
            if (!systemz_vreg_info(desc, mir, use, error, error_len))
                return false;
        }

        switch (instr.op) {
        case ANVIL_MIR_OP_CALL:
            if (instr.call_cc != ANVIL_CC_MVS) {
                return systemz_legal_fail(error, error_len, "%s call uses an unsupported calling convention", desc->name);
            }
            break;
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
        case ANVIL_MIR_OP_FPEXT:
        case ANVIL_MIR_OP_FPTRUNC:
            if (!systemz_verify_numeric_cast(mir, i, instr, error, error_len)) {
                return false;
            }
            if (!desc->has_64bit_gprs) {
                const anvil_mir_vreg_info_t *dst = anvil_mir_get_vreg_info(mir, instr.def);
                anvil_mir_vreg_t src_vreg = anvil_mir_get_instr_use(mir, i, 0);
                const anvil_mir_vreg_info_t *src = anvil_mir_get_vreg_info(mir, src_vreg);
                if ((src && src->reg_class == ANVIL_MIR_REG_GPR && src->size_bits > 32) || (dst && dst->reg_class == ANVIL_MIR_REG_GPR && dst->size_bits > 32)) {
                    return systemz_legal_fail(error, error_len, "%s cannot represent a 64-bit integer conversion vreg", desc->name);
                }
            }
            break;
        case ANVIL_MIR_OP_SMOD:
        case ANVIL_MIR_OP_UMOD:
            break;
        case ANVIL_MIR_OP_KEEPALIVE:
            break;
        case ANVIL_MIR_OP_CALL_RESULT:
        case ANVIL_MIR_OP_RET_VALUE_PART:
        case ANVIL_MIR_OP_INVALID:
            return systemz_legal_fail(error, error_len, "%s has illegal MachineIR opcode", desc->name);
        default:
            break;
        }
    }
    return true;
}

bool anvil_mainframe_regalloc_mir(anvil_mir_func_t *mir, anvil_mainframe_variant_t variant)
{
    const anvil_mainframe_target_desc_t *desc = anvil_mainframe_get_target_desc(variant);
    if (!desc || !mir)
        return false;
    if (!anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0))
        return false;

    anvil_regalloc_class_config_t configs[] = {{.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs},
                                               {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs}};
    anvil_regalloc_class_config_t scratch_configs[] = {{.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = (int)desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs},
                                                       {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = (int)desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs}};

    if (!anvil_regalloc_linear_scan_classes(mir, configs, sizeof(configs) / sizeof(configs[0]))) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs, sizeof(scratch_configs) / sizeof(scratch_configs[0]))) {
        return false;
    }
    return anvil_mainframe_verify_mir_legal(mir, variant, NULL, 0);
}
