#include "mir_legal.h"


const anvil_mir_vreg_info_t *anvil_mir_legal_vreg(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t index,
                                                  char *error, size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, vreg);
    if (!info) {
        anvil_format_failure(error, error_len, "%s MIR instruction %zu uses invalid vreg", target->name, index);
        return NULL;
    }
    if (info->reg_class >= ANVIL_MIR_REG_CLASS_COUNT || !(target->widths[info->reg_class] & ANVIL_MIR_WIDTH(info->size_bits))) {
        anvil_format_failure(error, error_len, "%s MIR instruction %zu uses unsupported vreg class/size (%u bits)", target->name, index, info->size_bits);
        return NULL;
    }
    if (info->has_fixed_reg && (info->fixed_phys_reg < 0 || info->fixed_phys_reg >= target->num_regs[info->reg_class])) {
        anvil_format_failure(error, error_len, "%s MIR instruction %zu uses invalid fixed register", target->name, index);
        return NULL;
    }
    return info;
}

static bool is_pointer(const anvil_mir_legal_target_t *target, const anvil_mir_vreg_info_t *info)
{
    return info && info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == target->pointer_bits;
}

bool anvil_mir_legal_pointer(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, size_t index, char *error,
                             size_t error_len)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_legal_vreg(target, mir, vreg, index, error, error_len);
    return info && (is_pointer(target, info) ||
                    anvil_format_failure(error, error_len, "%s MIR instruction %zu requires a %u-bit pointer operand", target->name, index, target->pointer_bits));
}

static bool same_class_and_size(const anvil_mir_vreg_info_t *a, const anvil_mir_vreg_info_t *b)
{
    return a && b && a->reg_class == b->reg_class && a->size_bits == b->size_bits;
}

bool anvil_mir_legal_call(const anvil_mir_legal_target_t *target, const anvil_mir_legal_call_t *rules, const anvil_mir_func_t *mir, size_t index,
                          const anvil_mir_instr_info_t *instr, char *error, size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG) {
        const anvil_mir_vreg_info_t *def = anvil_mir_legal_vreg(target, mir, instr->def, index, error, error_len);
        if (!def)
            return false;
        if ((rules->fixed_result && !def->has_fixed_reg) || (rules->result_ok && !rules->result_ok(target, def)))
            return anvil_format_failure(error, error_len, "%s MIR call %zu result must be fixed to ABI result register", target->name, index);
    }
    size_t first_arg = 0;
    if (!instr->symbol || !instr->symbol[0]) {
        if (instr->num_uses == 0)
            return anvil_format_failure(error, error_len, "%s MIR indirect call %zu requires a target register", target->name, index);
        const anvil_mir_vreg_info_t *callee = anvil_mir_legal_vreg(target, mir, anvil_mir_get_instr_use(mir, index, 0), index, error, error_len);
        if (!callee)
            return false;
        if (!is_pointer(target, callee) || (rules->fixed_target && !callee->has_fixed_reg) ||
            (rules->target_reg >= 0 && callee->fixed_phys_reg != rules->target_reg))
            return anvil_format_failure(error, error_len, "%s MIR indirect call %zu target must be a fixed pointer register", target->name, index);
        first_arg = 1;
    }
    for (size_t use = first_arg; use < instr->num_uses; use++) {
        const anvil_mir_vreg_info_t *arg = anvil_mir_legal_vreg(target, mir, anvil_mir_get_instr_use(mir, index, use), index, error, error_len);
        if (!arg)
            return false;
        if (rules->fixed_args && !arg->has_fixed_reg)
            return anvil_format_failure(error, error_len, "%s MIR call %zu argument %zu must use a fixed ABI register", target->name, index, use - first_arg);
        if (rules->arg_ok && !rules->arg_ok(target, arg))
            return anvil_format_failure(error, error_len, "%s MIR call %zu argument %zu has an invalid fixed ABI register", target->name, index, use - first_arg);
    }
    return true;
}

static bool binary(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                   size_t error_len)
{
    const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
    const anvil_mir_vreg_info_t *lhs = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0));
    const anvil_mir_vreg_info_t *rhs = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 1));
    if (instr->num_uses != 2 || !same_class_and_size(def, lhs) || !same_class_and_size(lhs, rhs))
        return anvil_format_failure(error, error_len, "%s MIR instruction %zu has incompatible binary operands", target->name, index);
    if (target->fp_vector_bits && def->reg_class == ANVIL_MIR_REG_FPR && def->size_bits == target->fp_vector_bits)
        return anvil_format_failure(error, error_len, "%s MIR scalar opcode at instruction %zu cannot operate on a vector", target->name, index);
    switch (instr->op) {
    case ANVIL_MIR_OP_ADD:
    case ANVIL_MIR_OP_SUB:
    case ANVIL_MIR_OP_MUL:
        return true;
    case ANVIL_MIR_OP_DIV:
    case ANVIL_MIR_OP_FDIV:
        if (def->reg_class == ANVIL_MIR_REG_GPR || def->reg_class == ANVIL_MIR_REG_FPR)
            return true;
        break;
    default:
        if (def->reg_class == ANVIL_MIR_REG_GPR)
            return true;
        break;
    }
    return anvil_format_failure(error, error_len, "%s MIR instruction %zu uses an illegal binary opcode/class pair", target->name, index);
}

static bool conversion(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                       size_t error_len)
{
    const anvil_mir_vreg_info_t *to = anvil_mir_get_vreg_info(mir, instr->def);
    const anvil_mir_vreg_info_t *from = instr->num_uses == 1 ? anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0)) : NULL;
    bool legal = false;
    if (to && from) {
        switch (instr->op) {
        case ANVIL_MIR_OP_SITOFP:
        case ANVIL_MIR_OP_UITOFP:
            legal = from->reg_class == ANVIL_MIR_REG_GPR && to->reg_class == ANVIL_MIR_REG_FPR;
            break;
        case ANVIL_MIR_OP_FPTOSI:
        case ANVIL_MIR_OP_FPTOUI:
            legal = from->reg_class == ANVIL_MIR_REG_FPR && to->reg_class == ANVIL_MIR_REG_GPR;
            break;
        case ANVIL_MIR_OP_FPEXT:
            legal = from->reg_class == ANVIL_MIR_REG_FPR && to->reg_class == ANVIL_MIR_REG_FPR && from->size_bits == 32 && to->size_bits == 64;
            break;
        default:
            legal = from->reg_class == ANVIL_MIR_REG_FPR && to->reg_class == ANVIL_MIR_REG_FPR && from->size_bits == 64 && to->size_bits == 32;
            break;
        }
    }
    return legal || anvil_format_failure(error, error_len, "%s MIR conversion %zu has incompatible source/destination types", target->name, index);
}

static bool access(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                   size_t error_len)
{
    size_t base = instr->op == ANVIL_MIR_OP_LOAD ? 0 : 1;
    if (!anvil_mir_legal_pointer(target, mir, anvil_mir_get_instr_use(mir, index, base), index, error, error_len))
        return false;
    if (!instr->index_scale)
        return true;
    /* An index is pointer-wide, or a 32-bit register the access extends. */
    anvil_mir_vreg_t index_vreg = anvil_mir_get_instr_use(mir, index, base + 1);
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, index_vreg);
    if (instr->index_extend && info && info->reg_class == ANVIL_MIR_REG_GPR && info->size_bits == 32)
        return true;
    return anvil_mir_legal_pointer(target, mir, index_vreg, index, error, error_len);
}

static bool stack_arg(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                      size_t error_len)
{
    int64_t align = target->stack_arg_align;
    if (!align) {
        anvil_mir_vreg_t value = instr->op == ANVIL_MIR_OP_CALL_STACK_ARG ? anvil_mir_get_instr_use(mir, index, 0) : instr->def;
        const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(mir, value);
        align = info ? (info->size_bits + 7) / 8 : 8;
        if (align > 8)
            align = 8;
    }
    if (instr->has_imm && instr->imm >= 0 && align > 0 && instr->imm % align == 0)
        return true;
    return anvil_format_failure(error, error_len, "%s MIR stack instruction %zu needs an aligned stack offset", target->name, index);
}

static bool legal_instr(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *instr, char *error,
                        size_t error_len)
{
    if (instr->def != ANVIL_MIR_NO_VREG && !anvil_mir_legal_vreg(target, mir, instr->def, index, error, error_len))
        return false;
    for (size_t use = 0; use < instr->num_uses; use++)
        if (!anvil_mir_legal_vreg(target, mir, anvil_mir_get_instr_use(mir, index, use), index, error, error_len))
            return false;
    if (target->fp_vector_bits && !anvil_vector_mir_operands_legal(mir, index, instr, target->fp_vector_bits))
        return anvil_format_failure(error, error_len, "%s MIR instruction %zu has no legal vector form", target->name, index);
    bool decided = false;
    if (target->hook && !target->hook(target, mir, index, instr, error, error_len, &decided))
        return false;
    if (decided)
        return true;

    anvil_cpu_features_t features = anvil_mir_func_cpu_features(mir);
    switch (instr->op) {
    case ANVIL_MIR_OP_SCALAR:
        return anvil_vector_isa_for_mir(target->scalar_isa, target->scalar_isa_count, mir, index, instr, features) ||
               anvil_format_failure(error, error_len, "%s MIR instruction %zu has no form on this CPU", target->name, index);
    case ANVIL_MIR_OP_VECTOR:
        return anvil_vector_isa_for_mir(target->vector_isa, target->vector_isa_count, mir, index, instr, features) ||
               anvil_format_failure(error, error_len, "%s MIR vector instruction %zu has no form on this CPU", target->name, index);
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
        return binary(target, mir, index, instr, error, error_len);
    case ANVIL_MIR_OP_ATOMIC:
        return instr->ir_op == ANVIL_OP_ATOMIC_FENCE || anvil_mir_legal_pointer(target, mir, anvil_mir_get_instr_use(mir, index, 0), index, error, error_len);
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_STORE:
        return access(target, mir, index, instr, error, error_len);
    case ANVIL_MIR_OP_SYMBOL_ADDR:
    case ANVIL_MIR_OP_FRAME_ADDR:
    case ANVIL_MIR_OP_DYN_ALLOCA:
        return is_pointer(target, anvil_mir_get_vreg_info(mir, instr->def)) ||
               anvil_format_failure(error, error_len, "%s MIR instruction %zu must define a %u-bit pointer", target->name, index, target->pointer_bits);
    case ANVIL_MIR_OP_INCOMING_STACK_ARG:
    case ANVIL_MIR_OP_CALL_STACK_ARG:
        return stack_arg(target, mir, index, instr, error, error_len);
    case ANVIL_MIR_OP_SELECT: {
        const anvil_mir_vreg_info_t *def = anvil_mir_get_vreg_info(mir, instr->def);
        if (instr->num_uses == 3 && def && (target->select_classes & (1u << def->reg_class)) &&
            same_class_and_size(def, anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 1))) &&
            same_class_and_size(def, anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 2))))
            return true;
        return anvil_format_failure(error, error_len, "%s MIR select %zu has values the target cannot select between", target->name, index);
    }
    case ANVIL_MIR_OP_SITOFP:
    case ANVIL_MIR_OP_UITOFP:
    case ANVIL_MIR_OP_FPTOSI:
    case ANVIL_MIR_OP_FPTOUI:
    case ANVIL_MIR_OP_FPEXT:
    case ANVIL_MIR_OP_FPTRUNC:
        return conversion(target, mir, index, instr, error, error_len);
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
        if (instr->num_uses != 2 ||
            same_class_and_size(anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 0)), anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, 1))))
            return true;
        return anvil_format_failure(error, error_len, "%s MIR compare %zu has incompatible operands", target->name, index);
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
    case ANVIL_MIR_OP_RET:
    case ANVIL_MIR_OP_BR:
    case ANVIL_MIR_OP_BR_COND:
    case ANVIL_MIR_OP_KEEPALIVE:
        return true;
    default:
        /* Calls, call results, VA_START and the return pseudos are the
         * target hook's to accept. */
        return anvil_format_failure(error, error_len, "%s MIR instruction %zu uses unsupported opcode", target->name, index);
    }
}

bool anvil_mir_verify_legal(const anvil_mir_legal_target_t *target, const anvil_mir_func_t *mir, char *error, size_t error_len)
{
    if (error && error_len > 0)
        error[0] = '\0';
    if (!anvil_mir_verify(mir, error, error_len))
        return false;
    for (size_t index = 0; index < anvil_mir_num_instrs(mir); index++) {
        anvil_mir_instr_info_t instr;
        if (!anvil_mir_get_instr_info(mir, index, &instr))
            return anvil_format_failure(error, error_len, "%s MIR instruction %zu is not inspectable", target->name, index);
        if (!legal_instr(target, mir, index, &instr, error, error_len))
            return false;
    }
    return true;
}

bool anvil_mir_allocate(anvil_mir_func_t *mir, const anvil_mir_allocation_t *target)
{
    const void *user = target->user;
    return mir && target->verify(mir, user) && (!target->annotate_clobbers || target->annotate_clobbers(mir, user)) &&
           anvil_mir_optimize_before_allocation(mir, target->address_mode, target->expensive_constant, target->mir_user) && anvil_mir_order_blocks(mir) &&
           anvil_mir_coalesce_copies(mir) && anvil_mir_split_early_return(mir) && (!target->annotate_printer || target->annotate_printer(mir, user)) &&
           target->verify(mir, user) && target->mark_registerless(mir, user) && anvil_mir_hoist_materialized_constants(mir) && anvil_mir_merge_block_constants(mir) &&
           anvil_regalloc_allocate(mir, target->configs, target->num_configs, target->scratch_configs, target->num_scratch_configs) && target->verify(mir, user);
}
