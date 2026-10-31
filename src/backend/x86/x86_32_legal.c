#include "x86_32_internal.h"
#include "x86_address.h"
#include "anvil/anvil_analysis.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool x86_legal_size_for_class(const anvil_mir_vreg_info_t *info)
{
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR) {
        return info->size_bits == 8 || info->size_bits == 16 || info->size_bits == 32;
    }
    if (info->reg_class == ANVIL_MIR_REG_FPR) {
        return info->size_bits == 32 || info->size_bits == 64;
    }
    return false;
}

static bool x86_legal_fixed_reg(const anvil_mir_vreg_info_t *info)
{
    if (!info || !info->has_fixed_reg)
        return true;
    if (info->fixed_phys_reg < 0)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR)
        return info->fixed_phys_reg <= 7;
    if (info->reg_class == ANVIL_MIR_REG_FPR)
        return info->fixed_phys_reg <= 7;
    return false;
}

static const anvil_mir_vreg_info_t *x86_legal_vreg_info(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        anvil_format_failure(error, error_len, "x86 MIR instruction %zu uses invalid vreg", instr_index);
        return NULL;
    }
    if (!x86_legal_size_for_class(info)) {
        anvil_format_failure(error, error_len, "x86 MIR instruction %zu uses unsupported vreg class/size", instr_index);
        return NULL;
    }
    if (!x86_legal_fixed_reg(info)) {
        anvil_format_failure(error, error_len, "x86 MIR instruction %zu uses invalid fixed register", instr_index);
        return NULL;
    }
    return info;
}

static bool x86_legal_pointer_operand(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t instr_index, char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = x86_legal_vreg_info(mir, vreg, instr_index, error, error_len);
    if (!info)
        return false;
    if (info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 32) {
        return true;
    }
    return anvil_format_failure(error, error_len, "x86 MIR instruction %zu requires a 32-bit pointer operand", instr_index);
}

static bool x86_legal_same_class_and_size(const anvil_mir_vreg_info_t *a, const anvil_mir_vreg_info_t *b)
{
    return a && b && a->reg_class == b->reg_class && a->size_bits == b->size_bits;
}

static bool x86_legal_binary(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    anvil_mir_vreg_t lhs = anvil_mir_get_instr_use(mir, instr_index, 0);
    anvil_mir_vreg_t rhs = anvil_mir_get_instr_use(mir, instr_index, 1);
    const anvil_mir_vreg_info_t *def = x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *lhs_info = x86_legal_vreg_info(mir, lhs, instr_index, error, error_len);
    const anvil_mir_vreg_info_t *rhs_info = x86_legal_vreg_info(mir, rhs, instr_index, error, error_len);
    if (!def || !lhs_info || !rhs_info)
        return false;

    bool is_compare = false;
    switch (instr->op) {
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
        is_compare = true;
        break;
    default:
        break;
    }

    if (is_compare) {
        if (!x86_legal_same_class_and_size(lhs_info, rhs_info)) {
            return anvil_format_failure(error, error_len, "x86 MIR compare %zu has incompatible operands", instr_index);
        }
        return true;
    }

    if (!x86_legal_same_class_and_size(def, lhs_info) || !x86_legal_same_class_and_size(lhs_info, rhs_info)) {
        return anvil_format_failure(error, error_len, "x86 MIR instruction %zu has incompatible binary operands", instr_index);
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
        if (def->reg_class == ANVIL_MIR_REG_GPR)
            return true;
        break;
    default:
        break;
    }

    return anvil_format_failure(error, error_len, "x86 MIR instruction %zu uses an illegal binary opcode/class pair", instr_index);
}

static bool x86_legal_call(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (!anvil_x86_get_cc_desc(instr->call_cc)) {
        return anvil_format_failure(error, error_len, "x86 MIR call %zu uses an unsupported calling convention", instr_index);
    }
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def = x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (!def)
            return false;
    }

    size_t arg_start = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0) {
            return anvil_format_failure(error, error_len, "x86 MIR indirect call %zu requires a target register", instr_index);
        }
        anvil_mir_vreg_t target = anvil_mir_get_instr_use(mir, instr_index, 0);
        const anvil_mir_vreg_info_t *target_info = x86_legal_vreg_info(mir, target, instr_index, error, error_len);
        if (!target_info)
            return false;
        if (target_info->reg_class != ANVIL_MIR_REG_GPR || target_info->size_bits != 32) {
            return anvil_format_failure(error, error_len, "x86 MIR indirect call %zu target must be a pointer register", instr_index);
        }
        arg_start = 1;
    }

    for (size_t u = arg_start; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x86_legal_vreg_info(mir, use, instr_index, error, error_len)) {
            return false;
        }
    }

    return true;
}

static bool x86_legal_instr(const anvil_mir_func_t *mir, size_t instr_index, const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG && !x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len)) {
        return false;
    }
    for (size_t u = 0; u < instr->num_uses; u++) {
        anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, instr_index, u);
        if (!x86_legal_vreg_info(mir, use, instr_index, error, error_len)) {
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
        return x86_legal_binary(mir, instr_index, instr, error, error_len);

    case ANVIL_MIR_OP_ATOMIC:
        return instr->ir_op == ANVIL_OP_ATOMIC_FENCE ||
               x86_legal_pointer_operand(mir, anvil_mir_get_instr_use(mir, instr_index, 0), instr_index, error, error_len);

    case ANVIL_MIR_OP_LOAD: {
        anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 0);
        return x86_legal_pointer_operand(mir, ptr, instr_index, error, error_len);
    }

    case ANVIL_MIR_OP_STORE: {
        anvil_mir_vreg_t ptr = anvil_mir_get_instr_use(mir, instr_index, 1);
        return x86_legal_pointer_operand(mir, ptr, instr_index, error, error_len);
    }

    case ANVIL_MIR_OP_VA_START: {
        /* Pointer to the first unnamed stack argument. */
        const anvil_mir_vreg_info_t *def = x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (instr->has_imm && instr->imm >= 0 && instr->imm % 4 == 0 && def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 32)
            return true;
        return anvil_format_failure(error, error_len, "x86 MIR va_start %zu needs a pointer result and a word-aligned argument offset", instr_index);
    }

    case ANVIL_MIR_OP_SYMBOL_ADDR:
    case ANVIL_MIR_OP_FRAME_ADDR:
    case ANVIL_MIR_OP_DYN_ALLOCA: {
        const anvil_mir_vreg_info_t *def = x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (def && def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 32) {
            return true;
        }
        return anvil_format_failure(error, error_len, "x86 MIR instruction %zu must define a 32-bit pointer", instr_index);
    }

    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        if (instr->has_imm && instr->imm >= 0 && (instr->imm % 4) == 0) {
            return true;
        }
        return anvil_format_failure(error, error_len, "x86 MIR stack instruction %zu needs an aligned stack offset", instr_index);

    case ANVIL_MIR_OP_CALL:
        return x86_legal_call(mir, instr_index, instr, error, error_len);

    case ANVIL_MIR_OP_SELECT:
        return anvil_format_failure(error, error_len, "x86 MIR select %zu must be lowered to a branch", instr_index);

    case ANVIL_MIR_OP_SCALAR:
        if (anvil_vector_isa_for_mir(x86_scalar_isa, x86_scalar_isa_count, mir, instr_index, instr, anvil_mir_func_cpu_features(mir)))
            return true;
        return anvil_format_failure(error, error_len, "x86 MIR instruction %zu has no form on this CPU", instr_index);

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
    case ANVIL_MIR_OP_RET:
    case ANVIL_MIR_OP_BR:
    case ANVIL_MIR_OP_BR_COND:
    case ANVIL_MIR_OP_KEEPALIVE:
    case ANVIL_MIR_OP_RET_VALUE_PART:
        return true;

    case ANVIL_MIR_OP_CALL_RESULT: {
        /* The emitter copies the call's second result location (EDX for a
         * 32-bit value, ST(0) for a floating one) into the assigned
         * register (allocated, or a spill scratch) right after the call. */
        const anvil_mir_vreg_info_t *def = x86_legal_vreg_info(mir, instr->def, instr_index, error, error_len);
        if (def && ((def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 32) || def->reg_class == ANVIL_MIR_REG_FPR))
            return true;
        return anvil_format_failure(error, error_len, "x86 MIR call result %zu must be a 32-bit or floating value", instr_index);
    }

    case ANVIL_MIR_OP_INVALID:
    default:
        break;
    }

    return anvil_format_failure(error, error_len, "x86 MIR instruction %zu uses unsupported opcode", instr_index);
}

bool anvil_x86_verify_mir_legal(const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    if (error && error_len > 0)
        error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len))
        return false;

    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, i, &instr)) {
            return anvil_format_failure(error, error_len, "x86 MIR instruction %zu is not inspectable", i);
        }
        if (!x86_legal_instr(mir, i, &instr, error, error_len)) {
            return false;
        }
    }

    return true;
}

bool anvil_x86_regalloc_mir(anvil_mir_func_t *mir)
{
    if (!mir)
        return false;

    const anvil_x86_cc_desc_t *desc = anvil_x86_get_cc_desc(ANVIL_CC_CDECL);
    if (!desc)
        return false;

    /* Bytes (and booleans) only in the registers with a byte form. */
    const uint64_t byte_regs = (UINT64_C(1) << X86_EAX) | (UINT64_C(1) << X86_ECX) | (UINT64_C(1) << X86_EDX) | (UINT64_C(1) << X86_EBX);
    anvil_regalloc_class_config_t configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = desc->num_alloc_gpr_regs, .phys_regs = desc->alloc_gpr_regs,
         .two_address = true, .narrow_bits = 8, .narrow_regs = byte_regs},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_alloc_fpr_regs, .phys_regs = desc->alloc_fpr_regs, .two_address = true},
    };
    /* gas32.c declares every write to them (x86_annotate_printer_registers,
     * the clobbers below); its FP units use only XMM4 and the frame. */
    anvil_regalloc_class_config_t scratch_configs[] = {
        {.reg_class = ANVIL_MIR_REG_GPR, .num_phys_regs = desc->num_scratch_gpr_regs, .phys_regs = desc->scratch_gpr_regs,
         .printer_declares_writes = true},
        {.reg_class = ANVIL_MIR_REG_FPR, .num_phys_regs = desc->num_scratch_fpr_regs, .phys_regs = desc->scratch_fpr_regs,
         .printer_declares_writes = true},
    };

    if (!anvil_x86_verify_mir_legal(mir, NULL, 0) || !anvil_mir_duplicate_branch_targets(mir, 4) || !anvil_mir_local_cse(mir) ||
        !anvil_mir_fold_address_arithmetic(mir, x86_address_mode, NULL))
        return false;

    /* What calls and atomics change: the callee's convention, and the
     * atomics' fixed EAX/ECX/EDX (gas32.c). */
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, index, &info))
            return false;
        uint64_t gpr = 0, fpr = 0;
        if (info.op == ANVIL_MIR_OP_ATOMIC) {
            gpr = desc->call_gpr_clobbers;
        } else if (info.op == ANVIL_MIR_OP_CALL) {
            const anvil_x86_cc_desc_t *callee = anvil_x86_get_cc_desc(info.call_cc);
            if (!callee)
                return false;
            gpr = callee->call_gpr_clobbers;
            fpr = callee->call_fpr_clobbers;
        }
        if ((gpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_GPR, info.clobbers[ANVIL_MIR_REG_GPR] | gpr)) ||
            (fpr && !anvil_mir_set_instr_clobbers(mir, index, ANVIL_MIR_REG_FPR, info.clobbers[ANVIL_MIR_REG_FPR] | fpr)))
            return false;
    }

    if (!anvil_mir_order_blocks(mir) || !anvil_mir_coalesce_copies(mir) || !x86_annotate_printer_registers(mir))
        return false;
    if (!anvil_x86_verify_mir_legal(mir, NULL, 0))
        return false;
    if (!anvil_regalloc_allocate(mir, configs, sizeof(configs) / sizeof(configs[0]), scratch_configs,
                                 sizeof(scratch_configs) / sizeof(scratch_configs[0])))
        return false;
    return anvil_x86_verify_mir_legal(mir, NULL, 0);
}
