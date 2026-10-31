#include "ir_lower.h"

static size_t lane_operation_arity(anvil_op_t operation)
{
    switch (operation) {
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
    case ANVIL_OP_FDIV:
    case ANVIL_OP_ADD:
    case ANVIL_OP_SUB:
    case ANVIL_OP_MUL:
    case ANVIL_OP_AND:
    case ANVIL_OP_OR:
    case ANVIL_OP_XOR:
    case ANVIL_OP_SHL:
    case ANVIL_OP_SHR:
    case ANVIL_OP_SAR:
    case ANVIL_OP_CMP_EQ:
    case ANVIL_OP_CMP_NE:
    case ANVIL_OP_CMP_LT:
    case ANVIL_OP_CMP_LE:
    case ANVIL_OP_CMP_GT:
    case ANVIL_OP_CMP_GE:
    case ANVIL_OP_CMP_ULT:
    case ANVIL_OP_CMP_ULE:
    case ANVIL_OP_CMP_UGT:
    case ANVIL_OP_CMP_UGE:
    case ANVIL_OP_FCMP:
    case ANVIL_OP_UMULH:
    case ANVIL_OP_SMULH:
    case ANVIL_OP_ACCUM_ZEXT:
    case ANVIL_OP_ACCUM_SEXT:
    case ANVIL_OP_ADD_SAT_U:
    case ANVIL_OP_ADD_SAT_S:
    case ANVIL_OP_SUB_SAT_U:
    case ANVIL_OP_SUB_SAT_S:
    case ANVIL_OP_ROTL:
        return 2;
    case ANVIL_OP_SELECT:
    case ANVIL_OP_FMA:
        return 3;
    case ANVIL_OP_SPLAT:
    case ANVIL_OP_NEG:
    case ANVIL_OP_NOT:
    case ANVIL_OP_FNEG:
    case ANVIL_OP_FABS:
    case ANVIL_OP_FSQRT:
        return 1;
    default:
        return anvil_op_is_reduction(operation) ? 1 : 0;
    }
}

bool anvil_vector_mir_form(anvil_ctx_t *ctx, const anvil_instr_t *instr, anvil_vector_mir_form_t *form)
{
    size_t arity = lane_operation_arity(instr->op);
    bool reduction = anvil_op_is_reduction(instr->op);
    if (!instr->result || !arity)
        return false;
    /* Single values: only operations the target prices in the scalar
     * shape; every other scalar lowers through the backend's own forms. */
    if (instr->result->type->kind != ANVIL_TYPE_VECTOR && !reduction) {
        if (instr->num_operands != arity ||
            !anvil_vector_shape_cost(ctx, instr->op, instr->result->type, ANVIL_VECTOR_SHAPE_SCALAR))
            return false;
        *form = (anvil_vector_mir_form_t){.operation = instr->op, .lane_bits = (unsigned)instr->result->type->size * 8,
                                          .num_uses = arity, .supported = true, .scalar = true};
        return true;
    }

    /* A comparison or a lane-mask select is asked about (and runs on) the
     * vectors it compares or selects; its result is their mask. */
    bool select = instr->op == ANVIL_OP_SELECT;
    bool compare = instr->op == ANVIL_OP_FCMP || (instr->op >= ANVIL_OP_CMP_EQ && instr->op <= ANVIL_OP_CMP_UGE);
    if (select && instr->num_operands == 3 && instr->operands[0]->type->kind != ANVIL_TYPE_VECTOR)
        return false;
    anvil_type_t *type = instr->result->type;
    if (instr->num_operands == arity && (select || compare || reduction))
        type = instr->operands[select ? 1 : 0]->type;
    if (type->kind != ANVIL_TYPE_VECTOR)
        return false;
    *form = (anvil_vector_mir_form_t){.operation = instr->op, .predicate = instr->fcmp_pred,
                                      .lane_bits = (unsigned)type->data.vector.element->size * 8};
    if (instr->num_operands != arity)
        return true;
    form->num_uses = arity;
    form->widening = instr->op == ANVIL_OP_ACCUM_ZEXT || instr->op == ANVIL_OP_ACCUM_SEXT
                         ? anvil_vector_widening(instr->result->type, instr->operands[1]->type)
                         : 0;
    anvil_vector_shape_t shape = form->widening ? anvil_vector_widen_shape(form->widening)
                                 : arity == 2 && instr->operands[1]->type->kind != ANVIL_TYPE_VECTOR && instr->op != ANVIL_OP_SPLAT
                                     ? ANVIL_VECTOR_SHAPE_UNIFORM
                                     : ANVIL_VECTOR_SHAPE_LANES;
    form->supported = anvil_vector_shape_cost(ctx, instr->op, type, shape) != 0;
    return true;
}

bool anvil_vector_mir_emit(anvil_mir_func_t *mir, const anvil_vector_mir_form_t *form, anvil_mir_vreg_t def, const anvil_mir_vreg_t *uses)
{
    if (!form->supported || def == ANVIL_MIR_NO_VREG)
        return false;
    for (size_t use = 0; use < form->num_uses; use++) {
        if (uses[use] == ANVIL_MIR_NO_VREG)
            return false;
    }
    if (form->scalar)
        return anvil_mir_add_scalar(mir, form->operation, def, uses, form->num_uses, form->lane_bits);
    return anvil_mir_add_vector(mir, form->operation, def, uses, form->num_uses, form->lane_bits) &&
           (form->operation != ANVIL_OP_FCMP || anvil_mir_set_instr_fcmp_pred(mir, anvil_mir_num_instrs(mir) - 1, form->predicate)) &&
           (!form->widening || anvil_mir_set_instr_widening(mir, anvil_mir_num_instrs(mir) - 1, form->widening));
}

bool anvil_cast_keeps_value(const anvil_instr_t *instr)
{
    return instr->op == ANVIL_OP_BITCAST && instr->num_operands == 1 && instr->result && instr->result->type->kind == ANVIL_TYPE_PTR &&
           instr->operands[0]->type->kind == ANVIL_TYPE_PTR;
}

anvil_mir_block_t anvil_mir_block_for(anvil_mir_func_t *mir, const anvil_block_t *block, bool first)
{
    anvil_mir_block_t mir_block = first ? anvil_mir_current_block(mir) : anvil_mir_add_block(mir, block->name);
    if (mir_block != ANVIL_MIR_NO_BLOCK && !anvil_mir_set_block_cold(mir, mir_block, block->cold))
        return ANVIL_MIR_NO_BLOCK;
    return mir_block;
}
