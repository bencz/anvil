/* The IR-to-MachineIR lowering every back end shares (ir_lower.h): the
 * function driver and the instructions whose MIR form does not depend on
 * the target, which leaves its calls, returns, VA_START and atomics to its
 * target_instr hook. */
#include "ir_lower.h"
#include "narrow_mir.h"

#include "anvil/anvil_analysis.h"

bool anvil_ir_low_bit(anvil_ir_lower_t *lower, anvil_mir_vreg_t value, anvil_mir_vreg_t def)
{
    anvil_mir_vreg_t one = anvil_ir_bool_vreg(lower);
    anvil_mir_vreg_t uses[] = {value, one};
    return value != ANVIL_MIR_NO_VREG && def != ANVIL_MIR_NO_VREG && one != ANVIL_MIR_NO_VREG && anvil_mir_add_instr_imm(lower->mir, ANVIL_MIR_OP_MOV, one, 1) &&
           anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_AND, def, uses, 2);
}

static bool lower_vector(anvil_ir_lower_t *lower, const anvil_instr_t *instr, const anvil_vector_mir_form_t *form)
{
    anvil_mir_vreg_t uses[ANVIL_VECTOR_MAX_USES] = {ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG, ANVIL_MIR_NO_VREG};
    for (size_t operand = 0; operand < form->num_uses; operand++)
        uses[operand] = anvil_ir_lower_value(lower, instr->operands[operand]);
    anvil_mir_vreg_t def = form->supported ? lower->vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
    return anvil_vector_mir_emit(lower->mir, form, def, uses) && anvil_lower_vreg_put(&lower->values, instr->result, def);
}

static bool lower_binary(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t lhs = anvil_ir_lower_value(lower, instr->operands[0]);
    /* A shift count held in a word pair counts by its low word. */
    anvil_mir_vreg_t rhs = anvil_ir_is_pair(lower, instr->operands[1]->type) ? anvil_ir_address_word(lower, instr->operands[1], false)
                                                                             : anvil_ir_lower_value(lower, instr->operands[1]);
    anvil_mir_vreg_t def = instr->result ? lower->vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
    if (lhs == ANVIL_MIR_NO_VREG || rhs == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
        return false;
    if (!anvil_ir_compare_op(mir_op) && !anvil_ir_match_operand_sizes(lower, &lhs, &rhs, def))
        return false;
    anvil_mir_vreg_t uses[] = {lhs, rhs};
    bool added;
    if (mir_op == ANVIL_MIR_OP_FCMP)
        added = anvil_mir_add_instr_imm_uses(lower->mir, mir_op, def, uses, 2, instr->fcmp_pred);
    else if (lower->narrowest_operation_bits)
        added = anvil_mir_add_binary_extending(lower->mir, mir_op, def, lhs, rhs, (uint16_t)lower->narrowest_operation_bits);
    else
        added = anvil_mir_add_instr(lower->mir, mir_op, def, uses, 2);
    return added && anvil_lower_vreg_put(&lower->values, instr->result, def);
}

static bool lower_unary(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    anvil_mir_vreg_t value = anvil_ir_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = instr->result ? lower->vreg_for_type(lower, instr->result->type) : ANVIL_MIR_NO_VREG;
    anvil_mir_vreg_t uses[] = {value};
    return value != ANVIL_MIR_NO_VREG && def != ANVIL_MIR_NO_VREG && anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1) &&
           anvil_lower_vreg_put(&lower->values, instr->result, def);
}

/* A conversion to i1: the low bit of the integer value (of the integer an
 * FP value converts to unsigned). */
static bool cast_to_bool(anvil_ir_lower_t *lower, anvil_mir_vreg_t value, anvil_mir_opcode_t mir_op, anvil_mir_vreg_t def)
{
    const anvil_mir_vreg_info_t *info = anvil_mir_get_vreg_info(lower->mir, value);
    if (info && info->reg_class == ANVIL_MIR_REG_FPR) {
        anvil_mir_vreg_t converted = anvil_mir_add_vreg_typed(lower->mir, ANVIL_MIR_REG_GPR, 32, false);
        anvil_mir_vreg_t uses[] = {value};
        if (mir_op != ANVIL_MIR_OP_FPTOUI || converted == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, mir_op, converted, uses, 1))
            return false;
        value = converted;
        info = anvil_mir_get_vreg_info(lower->mir, value);
    }
    if (!info || info->reg_class != ANVIL_MIR_REG_GPR)
        return false;
    if (info->size_bits > 8) {
        anvil_mir_vreg_t narrowed = anvil_ir_bool_vreg(lower);
        anvil_mir_vreg_t uses[] = {value};
        if (narrowed == ANVIL_MIR_NO_VREG || !anvil_mir_add_instr(lower->mir, ANVIL_MIR_OP_TRUNC, narrowed, uses, 1))
            return false;
        value = narrowed;
    }
    return anvil_ir_low_bit(lower, value, def);
}

static bool lower_cast(anvil_ir_lower_t *lower, const anvil_instr_t *instr, anvil_mir_opcode_t mir_op)
{
    if (!instr->result)
        return false;
    /* A pointer reinterpreted as another pointer is the same value (a
     * constant offset from a base stays one, for the accesses to fold). */
    if (anvil_cast_keeps_value(instr)) {
        anvil_mir_vreg_t base = ANVIL_MIR_NO_VREG;
        int64_t offset = 0;
        if (anvil_lower_address_get(&lower->addresses, instr->operands[0], &base, &offset))
            return anvil_lower_address_put(&lower->addresses, instr->result, base, offset);
        anvil_mir_vreg_t same = anvil_ir_lower_value(lower, instr->operands[0]);
        return same != ANVIL_MIR_NO_VREG && anvil_lower_vreg_put(&lower->values, instr->result, same);
    }
    anvil_mir_vreg_t value = anvil_ir_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t def = lower->vreg_for_type(lower, instr->result->type);
    if (value == ANVIL_MIR_NO_VREG || def == ANVIL_MIR_NO_VREG)
        return false;
    /* Narrow integers convert from their extended value. */
    value = anvil_mir_int_to_fp_source(lower->mir, mir_op, value, 32);
    if (value == ANVIL_MIR_NO_VREG)
        return false;
    if (instr->result->type->kind == ANVIL_TYPE_I1 && instr->operands[0]->type->kind != ANVIL_TYPE_I1)
        return cast_to_bool(lower, value, mir_op, def) && anvil_lower_vreg_put(&lower->values, instr->result, def);
    if (mir_op == ANVIL_MIR_OP_BITCAST) {
        /* A pointer and an integer of another width: a truncation or a
         * zero extension. */
        const anvil_mir_vreg_info_t *from = anvil_mir_get_vreg_info(lower->mir, value);
        const anvil_mir_vreg_info_t *to = anvil_mir_get_vreg_info(lower->mir, def);
        if (!from || !to)
            return false;
        if (from->size_bits != to->size_bits) {
            if (instr->op != ANVIL_OP_PTRTOINT && instr->op != ANVIL_OP_INTTOPTR)
                return false;
            mir_op = from->size_bits < to->size_bits ? ANVIL_MIR_OP_ZEXT : ANVIL_MIR_OP_TRUNC;
        }
    }
    anvil_mir_vreg_t uses[] = {value};
    return anvil_mir_add_instr(lower->mir, mir_op, def, uses, 1) && anvil_lower_vreg_put(&lower->values, instr->result, def);
}

/* An i1 is loaded and stored as a byte holding 0 or 1. */
static bool lower_load(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_vreg_t base;
    int64_t displacement;
    if (!instr->result || !anvil_ir_lower_memory_address(lower, instr->operands[0], &base, &displacement))
        return false;
    anvil_mir_vreg_t def = lower->vreg_for_type(lower, instr->result->type);
    if (def == ANVIL_MIR_NO_VREG)
        return false;
    /* Its sign unobserved: loaded zero-extended. */
    if (instr->sign_unobserved && !anvil_mir_set_vreg_signed(lower->mir, def, false))
        return false;
    bool is_bool = instr->result->type->kind == ANVIL_TYPE_I1;
    anvil_mir_vreg_t loaded = is_bool ? anvil_ir_bool_vreg(lower) : def;
    anvil_mir_vreg_t uses[] = {base};
    return loaded != ANVIL_MIR_NO_VREG && anvil_ir_add_access(lower, ANVIL_MIR_OP_LOAD, loaded, uses, 1, displacement) &&
           (!is_bool || anvil_ir_low_bit(lower, loaded, def)) && anvil_lower_vreg_put(&lower->values, instr->result, def);
}

static bool lower_store(anvil_ir_lower_t *lower, const anvil_instr_t *instr)
{
    anvil_mir_vreg_t value = anvil_ir_lower_value(lower, instr->operands[0]);
    anvil_mir_vreg_t base;
    int64_t displacement;
    if (value == ANVIL_MIR_NO_VREG || !anvil_ir_lower_memory_address(lower, instr->operands[1], &base, &displacement))
        return false;
    if (instr->operands[0]->type->kind == ANVIL_TYPE_I1) {
        anvil_mir_vreg_t bit = anvil_ir_bool_vreg(lower);
        if (!anvil_ir_low_bit(lower, value, bit))
            return false;
        value = bit;
    }
    anvil_mir_vreg_t uses[] = {value, base};
    return anvil_ir_add_access(lower, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, uses, 2, displacement);
}

bool anvil_ir_lower_instr(anvil_ir_lower_t *lower, anvil_instr_t *instr)
{
    if (instr->op == ANVIL_OP_NOP || instr->op == ANVIL_OP_PHI)
        return true;
    /* Vector arithmetic and broadcasts, and the scalar operations the
     * target's table prices (FMA, FSQRT), where the target has them. */
    anvil_vector_mir_form_t vector;
    if (anvil_vector_mir_form(lower->func->owner_ctx, instr, &vector))
        return lower_vector(lower, instr, &vector);
    if (anvil_ir_pair_instr(lower, instr))
        return anvil_ir_lower_pair_instr(lower, instr);
    anvil_mir_opcode_t mir_op;
    if (instr->num_operands == 2 && anvil_ir_binary_op(instr->op, &mir_op))
        return lower_binary(lower, instr, mir_op);
    if (instr->num_operands == 1 && anvil_ir_unary_op(instr->op, &mir_op))
        return lower_unary(lower, instr, mir_op);
    if (instr->num_operands == 1 && anvil_ir_cast_op(instr->op, &mir_op))
        return lower_cast(lower, instr, mir_op);
    switch (instr->op) {
    case ANVIL_OP_ALLOCA:
        return anvil_ir_lower_alloca(lower, instr);
    case ANVIL_OP_GEP:
        return anvil_ir_lower_gep(lower, instr);
    case ANVIL_OP_STRUCT_GEP:
        return anvil_ir_lower_struct_gep(lower, instr);
    case ANVIL_OP_SELECT:
        return anvil_ir_lower_select(lower, instr);
    case ANVIL_OP_LOAD:
        return instr->num_operands == 1 && lower_load(lower, instr);
    case ANVIL_OP_STORE:
        return instr->num_operands == 2 && lower_store(lower, instr);
    case ANVIL_OP_BR:
        return anvil_ir_lower_branch(lower, instr);
    case ANVIL_OP_BR_COND:
        return anvil_ir_lower_cond_branch(lower, instr);
    case ANVIL_OP_SWITCH:
        return anvil_ir_lower_switch(lower, instr);
    default:
        return lower->target_instr(lower, instr);
    }
}

anvil_mir_func_t *anvil_ir_lower_function(anvil_ir_lower_t *lower)
{
    anvil_func_t *func = lower->func;
    lower->mir = anvil_mir_func_create(func->name);
    if (!lower->mir)
        return NULL;
    anvil_mir_func_set_linkage(lower->mir, func->linkage);
    anvil_mir_func_set_cpu_features(lower->mir, anvil_ctx_get_cpu_features(func->owner_ctx));
    anvil_opt_cfg_t cfg;
    bool built = anvil_opt_cfg_build(func, &cfg);
    bool ok = built && anvil_opt_cfg_build_layout(&cfg) && anvil_lower_create_blocks(lower->mir, func, &lower->blocks) &&
              anvil_mir_set_current_block(lower->mir, anvil_lower_block_get(&lower->blocks, func->entry)) && lower->lower_params(lower) &&
              anvil_ir_prepare_phis(lower);
    for (size_t rank = 0; ok && rank < cfg.count; rank++) {
        anvil_block_t *block = cfg.blocks[cfg.layout[rank]];
        anvil_mir_block_t mir_block = anvil_lower_block_get(&lower->blocks, block);
        ok = mir_block != ANVIL_MIR_NO_BLOCK && anvil_mir_set_current_block(lower->mir, mir_block);
        for (anvil_instr_t *instr = block->first; ok && instr; instr = instr->next) {
            size_t first = anvil_mir_num_instrs(lower->mir);
            ok = anvil_ir_lower_instr(lower, instr) && anvil_mir_annotate_memory(lower->mir, first, &instr->memory_access) &&
                 (instr->op != ANVIL_OP_CALL || anvil_mir_annotate_call_effects(lower->mir, first, anvil_instr_call_effects(instr)));
        }
    }
    if (built)
        anvil_opt_cfg_destroy(&cfg);
    anvil_ir_lower_destroy(lower);
    if (!ok) {
        anvil_mir_func_destroy(lower->mir);
        lower->mir = NULL;
    }
    return lower->mir;
}
