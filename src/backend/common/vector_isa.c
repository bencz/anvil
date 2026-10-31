#include "vector_isa.h"

static bool lane_kind_matches(anvil_vector_lane_kind_t lanes, bool floating)
{
    return lanes == ANVIL_VECTOR_LANES_ANY || (lanes == ANVIL_VECTOR_LANES_FP) == floating;
}

const anvil_vector_isa_t *anvil_vector_isa_find(const anvil_vector_isa_t *table, size_t count, const anvil_vector_key_t *key,
                                                anvil_cpu_features_t features)
{
    for (size_t entry = 0; entry < count; entry++) {
        const anvil_vector_isa_t *isa = &table[entry];
        if (isa->operation == key->operation && (isa->lane_bits == 0 || isa->lane_bits == key->lane_bits) &&
            lane_kind_matches(isa->lanes, key->floating) && isa->shape == key->shape && (features & isa->features) == isa->features)
            return isa;
    }
    return NULL;
}

unsigned anvil_vector_isa_cost(const anvil_vector_isa_t *table, size_t count, size_t vector_bytes, anvil_ctx_t *ctx, anvil_op_t operation,
                               anvil_type_t *type, anvil_vector_shape_t shape)
{
    bool scalar = shape == ANVIL_VECTOR_SHAPE_SCALAR;
    if (!type || (scalar ? type->kind == ANVIL_TYPE_VECTOR : type->kind != ANVIL_TYPE_VECTOR || type->size != vector_bytes))
        return 0;
    anvil_type_t *element = scalar ? type : type->data.vector.element;
    anvil_vector_key_t key = {
        .operation = operation,
        .lane_bits = (unsigned)element->size * 8,
        .floating = anvil_type_is_floating(element),
        .shape = operation == ANVIL_OP_SPLAT ? ANVIL_VECTOR_SHAPE_LANES : shape,
    };
    const anvil_vector_isa_t *isa = anvil_vector_isa_find(table, count, &key, anvil_ctx_get_cpu_features(ctx));
    return isa ? isa->cost : 0;
}

static bool is_floating_operation(anvil_op_t operation)
{
    switch (operation) {
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
    case ANVIL_OP_FDIV:
    case ANVIL_OP_FNEG:
    case ANVIL_OP_FABS:
    case ANVIL_OP_FCMP:
    case ANVIL_OP_REDUCE_FADD:
    case ANVIL_OP_FMA:
    case ANVIL_OP_FSQRT:
        return true;
    default:
        return false;
    }
}

bool anvil_vector_mir_key(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, anvil_vector_key_t *key)
{
    if (info->op == ANVIL_MIR_OP_SCALAR && info->has_imm) {
        *key = (anvil_vector_key_t){.operation = info->ir_op, .lane_bits = (unsigned)info->imm,
                                    .floating = is_floating_operation(info->ir_op), .shape = ANVIL_VECTOR_SHAPE_SCALAR};
        return true;
    }
    if (info->op != ANVIL_MIR_OP_VECTOR || !info->has_imm || info->num_uses == 0)
        return false;
    const anvil_mir_vreg_info_t *last = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, info->num_uses - 1));
    if (!last)
        return false;
    bool scalar_last = last->size_bits != 128;
    *key = (anvil_vector_key_t){
        .operation = info->ir_op,
        .lane_bits = (unsigned)info->imm,
        .floating = info->ir_op == ANVIL_OP_SPLAT ? last->reg_class == ANVIL_MIR_REG_FPR : is_floating_operation(info->ir_op),
        .shape = info->widening           ? anvil_vector_widen_shape(info->widening)
                 : scalar_last && info->ir_op != ANVIL_OP_SPLAT ? ANVIL_VECTOR_SHAPE_UNIFORM
                                                                 : ANVIL_VECTOR_SHAPE_LANES,
    };
    return true;
}

bool anvil_vector_isa_print(anvil_strbuf_t *code, const char *mnemonic, const char *const *operands, size_t count)
{
    for (size_t operand = 0; operand < count; operand++) {
        if (!operands[operand])
            return false;
    }
    anvil_strbuf_appendf(code, "\t%s", mnemonic);
    for (size_t operand = 0; operand < count; operand++)
        anvil_strbuf_appendf(code, "%s%s", operand ? ", " : " ", operands[operand]);
    anvil_strbuf_append(code, "\n");
    return true;
}

bool anvil_vector_mir_is_mask(const anvil_mir_func_t *mir, size_t index, anvil_mir_vreg_t vreg)
{
    anvil_mir_instr_info_t user;
    if (!anvil_mir_get_instr_info(mir, index, &user))
        return false;
    for (size_t at = index; at-- > 0;) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, at, &info) || info.block != user.block)
            return false;
        if (info.def == vreg)
            return info.op == ANVIL_MIR_OP_VECTOR &&
                   (info.ir_op == ANVIL_OP_FCMP || (info.ir_op >= ANVIL_OP_CMP_EQ && info.ir_op <= ANVIL_OP_CMP_UGE));
    }
    return false;
}

const anvil_vector_isa_t *anvil_vector_isa_for_mir(const anvil_vector_isa_t *table, size_t count, const anvil_mir_func_t *mir, size_t index,
                                                   const anvil_mir_instr_info_t *info, anvil_cpu_features_t features)
{
    anvil_vector_key_t key;
    return anvil_vector_mir_key(mir, index, info, &key) ? anvil_vector_isa_find(table, count, &key, features) : NULL;
}
