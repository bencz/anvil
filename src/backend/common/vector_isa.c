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

/* Whether every definition of `vreg` leaves each lane all ones or zero: a
 * vector comparison, an AND/OR/XOR of such values, a copy of one, or a
 * splat of a constant that is all ones or zero in the lane's width. */
static bool mask_value(const anvil_mir_func_t *mir, anvil_mir_vreg_t vreg, unsigned depth)
{
    if (depth > 4)
        return false;
    bool defined = false;
    size_t count = anvil_mir_num_instrs(mir);
    for (size_t at = 0; at < count; at++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, at, &info) || info.def != vreg)
            continue;
        defined = true;
        bool compare = info.op == ANVIL_MIR_OP_VECTOR &&
                       (info.ir_op == ANVIL_OP_FCMP || (info.ir_op >= ANVIL_OP_CMP_EQ && info.ir_op <= ANVIL_OP_CMP_UGE));
        if (compare)
            continue;
        if (info.op == ANVIL_MIR_OP_VECTOR && (info.ir_op == ANVIL_OP_AND || info.ir_op == ANVIL_OP_OR || info.ir_op == ANVIL_OP_XOR) &&
            info.num_uses == 2 && mask_value(mir, anvil_mir_get_instr_use(mir, at, 0), depth + 1) &&
            mask_value(mir, anvil_mir_get_instr_use(mir, at, 1), depth + 1))
            continue;
        if (info.op == ANVIL_MIR_OP_COPY && info.num_uses == 1 && mask_value(mir, anvil_mir_get_instr_use(mir, at, 0), depth + 1))
            continue;
        if (info.op == ANVIL_MIR_OP_VECTOR && info.ir_op == ANVIL_OP_SPLAT && info.num_uses == 1) {
            /* The splat lane: one constant definition, all ones or zero. */
            anvil_mir_vreg_t lane = anvil_mir_get_instr_use(mir, at, 0);
            const anvil_mir_vreg_info_t *lane_info = anvil_mir_get_vreg_info(mir, lane);
            unsigned bits = lane_info ? lane_info->size_bits : 0;
            uint64_t ones = bits >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
            size_t constants = 0;
            bool all_or_none = bits > 0;
            for (size_t def = 0; def < count && all_or_none; def++) {
                anvil_mir_instr_info_t source;
                if (!anvil_mir_get_instr_info(mir, def, &source) || source.def != lane)
                    continue;
                constants++;
                uint64_t value = (uint64_t)source.imm & ones;
                all_or_none = source.op == ANVIL_MIR_OP_MOV && source.has_imm && source.num_uses == 0 && !source.symbol &&
                              (value == 0 || value == ones);
            }
            if (all_or_none && constants == 1)
                continue;
        }
        return false;
    }
    return defined;
}

bool anvil_vector_mir_is_mask(const anvil_mir_func_t *mir, size_t index, anvil_mir_vreg_t vreg)
{
    anvil_mir_instr_info_t user;
    return anvil_mir_get_instr_info(mir, index, &user) && mask_value(mir, vreg, 0);
}

const anvil_vector_isa_t *anvil_vector_isa_for_mir(const anvil_vector_isa_t *table, size_t count, const anvil_mir_func_t *mir, size_t index,
                                                   const anvil_mir_instr_info_t *info, anvil_cpu_features_t features)
{
    anvil_vector_key_t key;
    return anvil_vector_mir_key(mir, index, info, &key) ? anvil_vector_isa_find(table, count, &key, features) : NULL;
}

bool anvil_vector_mir_operands_legal(const anvil_mir_func_t *mir, size_t index, const anvil_mir_instr_info_t *info, uint16_t vector_bits)
{
    const anvil_mir_vreg_info_t *def = info->def != ANVIL_MIR_NO_VREG ? anvil_mir_get_vreg_info(mir, info->def) : NULL;
    bool vector = def && def->size_bits == vector_bits;
    for (size_t operand = 0; operand < info->num_uses && !vector; operand++) {
        const anvil_mir_vreg_info_t *use = anvil_mir_get_vreg_info(mir, anvil_mir_get_instr_use(mir, index, operand));
        vector = use && use->size_bits == vector_bits;
    }
    if (!vector)
        return true;
    switch (info->op) {
    case ANVIL_MIR_OP_VECTOR:
    case ANVIL_MIR_OP_LOAD:
    case ANVIL_MIR_OP_STORE:
    case ANVIL_MIR_OP_COPY:
    case ANVIL_MIR_OP_SPILL_LOAD:
    case ANVIL_MIR_OP_SPILL_STORE:
    case ANVIL_MIR_OP_KEEPALIVE:
        return true;
    default:
        return false;
    }
}
