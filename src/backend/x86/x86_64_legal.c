#include "x86_64_internal.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool x64_legal_fail(char *error, size_t error_len, const char *fmt, ...)
{
    if (error && error_len > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(error, error_len, fmt, args);
        va_end(args);
    }
    return false;
}

static bool x64_legal_size_for_class(const anvil_mir_vreg_info_t *info)
{
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        return info->size_bits == 8 || info->size_bits == 16 || info->size_bits == 32 || info->size_bits == 64;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64 || info->size_bits == 128;
    }
    return false;
}

static bool x64_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg)
        return true;
    if (info->fixed_phys_reg < 0)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR)
        return info->fixed_phys_reg <= 15;
    if (info->reg_class == ANVIL_MIR_REG_FPR)
        return info->fixed_phys_reg <= 15;
    return false;
}

static const anvil_mir_vreg_info_t *x64_legal_vreg_info(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu uses invalid vreg", instr_index);
        return NULL;
    }
    if (!x64_legal_size_for_class(info)) {
        x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu uses unsupported vreg class/size", instr_index);
        return NULL;
    }
    if (!x64_legal_fixed_reg(info)) {
        x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu uses invalid fixed register", instr_index);
        return NULL;
    }
    return info;
}

static bool x64_legal_pointer_operand(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = x64_legal_vreg_info(mir, vreg, instr_index, error, error_len);
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 64) {
        return true;
    }
    return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu requires a 64-bit pointer operand", instr_index);
}

static bool x64_legal_same_class_and_size(const anvil_mir_vreg_info_t *a, const anvil_mir_vreg_info_t *b)
{
    return a && b && a->reg_class == b->reg_class && a->size_bits == b->size_bits;
}

static bool x64_legal_binary(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def = x64_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info = x64_legal_vreg_info(mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info = x64_legal_vreg_info(mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info)
        return false;

    if (!x64_legal_same_class_and_size(def, lhs_info) || !x64_legal_same_class_and_size(lhs_info, rhs_info)) {
        return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu has incompatible binary operands", instr_index);
    }

    bool fp_op = def->reg_class == ANVIL_MIR_REG_FPR;
    if (def->size_bits == 128)
        return x64_legal_fail(error, error_len, "scalar opcode at instruction %zu cannot operate on a vector", instr_index);

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
        if (def->reg_class == ANVIL_MIR_REG_GPR)
            return true;
        break;
    default:
        break;
    }

    return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu uses an illegal binary opcode/class pair", instr_index);
}

static bool x64_legal_call(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->call_cc != ANVIL_CC_SYSV && instr->call_cc != ANVIL_CC_WIN64) {
        return x64_legal_fail(error, error_len, "x86-64 MIR call %zu uses an unsupported calling convention", instr_index);
    }
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def = x64_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (!def)
            return false;
        if (!def->has_fixed_reg) {
            return x64_legal_fail(error, error_len, "x86-64 MIR call %zu result must be fixed to ABI result register", instr_index);
        }
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return x64_legal_fail(error, error_len, "x86-64 MIR indirect call %zu requires a target register", instr_index);
        }

        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info = x64_legal_vreg_info(mir, target, instr_index, error, error_len);
        if (!target_info)
            return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR || target_info->size_bits != 64 || !target_info->has_fixed_reg) {
            return x64_legal_fail(error, error_len, "x86-64 MIR indirect call %zu target must be a fixed pointer register", instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        const anvil_mir_vreg_info_t *info = x64_legal_vreg_info(mir, use, instr_index, error, error_len);
        if (!info)
            return false;
        if (!info->has_fixed_reg) {
            return x64_legal_fail(error, error_len, "x86-64 MIR call %zu argument %zu must use a fixed ABI register", instr_index, u - arg_start);
        }
    }

    return true;
}

static bool x64_legal_instr(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG && !x64_legal_vreg_info(mir, instr->def, instr_index, error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x64_legal_vreg_info(mir, use, instr_index, error, error_len)) {
            return false;
        }
    }

    bool vector = instr->def != ANVIL_MIR_NO_VREG && anvil_mir_get_vreg_info(mir, instr->def)->size_bits == 128;
    for (size_t operand = 0; operand < instr->num_uses; operand++)
        vector |= anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, instr_index, operand))->size_bits == 128;

    if (vector) {
        switch (instr->op) {
        case ANVIL_MIR_OP_VECTOR_FADD:
        case ANVIL_MIR_OP_VECTOR_FSUB:
        case ANVIL_MIR_OP_VECTOR_FMUL:
        case ANVIL_MIR_OP_VECTOR_FDIV:
        case ANVIL_MIR_OP_LOAD:
        case ANVIL_MIR_OP_STORE:
        case ANVIL_MIR_OP_COPY:
        case ANVIL_MIR_OP_SPILL_LOAD:
        case ANVIL_MIR_OP_SPILL_STORE:
        case ANVIL_MIR_OP_KEEPALIVE:
            break;
        default:
            return x64_legal_fail(error, error_len, "instruction %zu has no legal vector form", instr_index);
        }
    }

    switch (instr->op) {
    case ANVIL_MIR_OP_VECTOR_FADD:
    case ANVIL_MIR_OP_VECTOR_FSUB:
    case ANVIL_MIR_OP_VECTOR_FMUL:
    case ANVIL_MIR_OP_VECTOR_FDIV:
        return true;
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
        return x64_legal_binary(mir, instr_index, instr, error, error_len);

    case ANVIL_MIR_OP_ATOMIC:
        return instr->atomic_op == ANVIL_OP_ATOMIC_FENCE || x64_legal_pointer_operand(mir, anvil_mir_get_instr_use(mir, instr_index, 0), instr_index, error, error_len);

    case ANVIL_MIR_OP_LOAD: {
        anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
        return x64_legal_pointer_operand(mir, ptr, instr_index, error, error_len);
    }

    case ANVIL_MIR_OP_STORE: {
        anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
        return x64_legal_pointer_operand(mir, ptr, instr_index, error, error_len);
    }

    case ANVIL_MIR_OP_SYMBOL_ADDR:
    case ANVIL_MIR_OP_FRAME_ADDR:
    case ANVIL_MIR_OP_DYN_ALLOCA: {
        const anvil_mir_vreg_info_t *def = x64_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 64) {
            return true;
        }
        return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu must define a 64-bit pointer", instr_index);
    }

    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        if (instr->has_imm && instr->imm >= 0 && (instr->imm % 8) == 0) {
            return true;
        }
        return x64_legal_fail(error, error_len, "x86-64 MIR stack instruction %zu needs an aligned stack offset", instr_index);

    case ANVIL_MIR_OP_VA_START: {
        if (!x64_legal_pointer_operand(mir, instr->def, instr_index, error, error_len))
            return false;
        if (instr->frame_slot < 0)
            return instr->has_imm && instr->imm >= 0;

        anvil_mir_frame_slot_info_t slot;
        return anvil_mir_get_frame_slot_info(mir, instr->frame_slot, &slot) && slot.size_bits >= 208 * 8 && slot.align_bytes >= 8 && instr->named_gpr <= 6 && instr->named_fpr <= 8 &&
               instr->named_stack_bytes <= INT_MAX - 16 && !(instr->named_stack_bytes % 8);
    }

    case ANVIL_MIR_OP_CALL:
        return x64_legal_call(mir, instr_index, instr, error, error_len);

    case ANVIL_MIR_OP_SELECT:
        return x64_legal_fail(error, error_len, "x86-64 MIR select %zu must be lowered to a branch", instr_index);

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
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
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
    case ANVIL_MIR_OP_KEEPALIVE:
        return true;

    case ANVIL_MIR_OP_INVALID:
    default:
        break;
    }

    return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu uses unsupported opcode", instr_index);
}

bool anvil_x86_64_verify_mir_legal(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    if (error && error_len > 0)
        error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len))
        return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return x64_legal_fail(error, error_len, "x86-64 MIR instruction %zu is not inspectable", i);
        }
        if (!x64_legal_instr(mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_x86_64_regalloc_mir_abi(anvil_mir_func_t *mir, anvil_abi_t abi)
{
    if (!mir)
        return false;

    const anvil_x64_abi_desc_t *desc = anvil_x64_get_abi_desc(abi);
    if (!desc)
        return false;

    anvil_regalloc_class_config_t configs[2];
    size_t num_configs = 0;
    configs[num_configs].reg_class = ANVIL_MIR_REG_GPR;
    configs[num_configs].num_phys_regs = desc->num_alloc_gpr_regs;
    configs[num_configs].phys_regs = desc->alloc_gpr_regs;
    num_configs++;
    configs[num_configs].reg_class = ANVIL_MIR_REG_FPR;
    configs[num_configs].num_phys_regs = desc->num_alloc_fpr_regs;
    configs[num_configs].phys_regs = desc->alloc_fpr_regs;
    num_configs++;

    anvil_regalloc_class_config_t scratch_configs[2];
    size_t num_scratch_configs = 0;
    scratch_configs[num_scratch_configs].reg_class = ANVIL_MIR_REG_GPR;
    scratch_configs[num_scratch_configs].num_phys_regs = desc->num_scratch_gpr_regs;
    scratch_configs[num_scratch_configs].phys_regs = desc->scratch_gpr_regs;
    num_scratch_configs++;
    scratch_configs[num_scratch_configs].reg_class = ANVIL_MIR_REG_FPR;
    scratch_configs[num_scratch_configs].num_phys_regs = desc->num_scratch_fpr_regs;
    scratch_configs[num_scratch_configs].phys_regs = desc->scratch_fpr_regs;
    num_scratch_configs++;

    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0))
        return false;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;

        if (info.op == ANVIL_MIR_OP_ATOMIC) {
            uint64_t scratch = (UINT64_C(1) << X64_RAX) | (UINT64_C(1) << X64_RCX) | (UINT64_C(1) << X64_RDX) | (UINT64_C(1) << X64_R11);
            if (!anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | scratch))
                return false;
        }

        if (info.op == ANVIL_MIR_OP_VA_START && info.frame_slot >= 0 && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | (UINT64_C(1) << X64_R11)))
            return false;

        if (info.op != ANVIL_MIR_OP_CALL)
            continue;

        const anvil_x64_abi_desc_t *callee = anvil_x64_get_abi_desc(info.call_cc == ANVIL_CC_WIN64 ? ANVIL_ABI_WIN64 : ANVIL_ABI_SYSV);
        if (!anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | callee->call_gpr_clobbers) ||
            !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | callee->call_fpr_clobbers))
            return false;
    }

    if (!anvil_mir_coalesce_copies(mir))
        return false;
    if (!anvil_x86_64_verify_mir_legal(mir, NULL, 0))
        return false;
    if (!anvil_regalloc_linear_scan_classes(mir, configs, num_configs)) {
        return false;
    }
    if (!anvil_mir_materialize_spills(mir, scratch_configs, num_scratch_configs)) {
        return false;
    }
    return anvil_x86_64_verify_mir_legal(mir, NULL, 0);
}

bool anvil_x86_64_regalloc_mir(anvil_mir_func_t *mir)
{
    return anvil_x86_64_regalloc_mir_abi(mir, ANVIL_ABI_SYSV);
}
