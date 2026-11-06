#include "anvil/anvil_analysis.h"

/* base + index * scale + offset, size bytes; index NULL: base + offset. */
typedef struct {
    const anvil_value_t *base;
    const anvil_value_t *index;
    size_t scale;
    int64_t offset;
    size_t size;
    bool bounded;
    bool local;
} memory_location_t;

static bool add_gep_offset(const anvil_instr_t *instr, int64_t *offset)
{
    if (instr->op == ANVIL_OP_STRUCT_GEP)
    {
        size_t field = (size_t)instr->operands[1]->data.u;
        if (field >= instr->aux_type->data.struc.num_fields)
            return false;

        size_t displacement = instr->aux_type->data.struc.offsets[field];
        return displacement <= INT64_MAX && anvil_gep_accumulate_offset(offset, (int64_t)displacement);
    }

    anvil_type_t *current = instr->aux_type;
    for (size_t index = 1; index < instr->num_operands; index++)
    {
        anvil_value_t *value = instr->operands[index];
        anvil_gep_step_t step;
        int64_t displacement;
        if (value->kind != ANVIL_VAL_CONST_INT || !anvil_gep_analyze_step(&current, value, index - 1, &step) ||
            !anvil_gep_const_step_offset(&step, value, &displacement) || !anvil_gep_accumulate_offset(offset, displacement))
            return false;
    }

    return true;
}

/* The address `pointer` is computed from by one step of address
 * arithmetic (a constant symbol address, a pointer bitcast, a GEP), or NULL. */
static const anvil_value_t *address_source(const anvil_value_t *pointer)
{
    if (pointer->kind == ANVIL_VAL_CONST_GEP || pointer->kind == ANVIL_VAL_CONST_SYMBOL_ADDR)
        return pointer->data.reloc.symbol;
    const anvil_instr_t *instr = pointer->kind == ANVIL_VAL_INSTR ? pointer->data.instr : NULL;
    if (!instr || !instr->num_operands)
        return NULL;
    if (instr->op == ANVIL_OP_GEP || instr->op == ANVIL_OP_STRUCT_GEP || (instr->op == ANVIL_OP_BITCAST && instr->operands[0]->type->kind == ANVIL_TYPE_PTR))
        return instr->operands[0];
    return NULL;
}

bool anvil_memory_index_distributes(const anvil_instr_t *term, size_t pointer_size)
{
    return term->result && (term->result->type->size == pointer_size || term->no_wrap);
}

bool anvil_memory_index_term(const anvil_instr_t *term, size_t scale, size_t pointer_size, int64_t *amount)
{
    const anvil_value_t *constant = term->num_operands == 2 ? term->operands[1] : NULL;
    if ((term->op != ANVIL_OP_ADD && term->op != ANVIL_OP_SUB) || !constant || constant->kind != ANVIL_VAL_CONST_INT ||
        !anvil_memory_index_distributes(term, pointer_size) || !scale || scale > (size_t)INT64_MAX)
        return false;
    unsigned bits = (unsigned)term->result->type->size * 8;
    if (!bits || bits > 64)
        return false;
    uint64_t raw = bits == 64 ? constant->data.u : constant->data.u & ((UINT64_C(1) << bits) - 1);
    int64_t value;
    if (term->result->type->is_signed && bits < 64 && (raw >> (bits - 1)) & 1)
        value = (int64_t)(raw | ~((UINT64_C(1) << bits) - 1));
    else if (!term->result->type->is_signed && raw > (uint64_t)INT64_MAX)
        return false;
    else
        value = (int64_t)raw;
    if (value == INT64_MIN || (value < 0 ? -value : value) > INT64_MAX / (int64_t)scale)
        return false;
    *amount = term->op == ANVIL_OP_SUB ? -value : value;
    return true;
}

/* A GEP whose only variable index is its last one: that index with its
 * constant terms peeled into *offset, its scale, and the other indices'
 * offset.  Two addresses with one base and one index value are then apart
 * by their offsets. */
static bool variable_index(const anvil_instr_t *instr, int64_t *offset, const anvil_value_t **index, size_t *scale)
{
    if (instr->op != ANVIL_OP_GEP || instr->num_operands < 2)
        return false;
    anvil_type_t *current = instr->aux_type;
    size_t last = instr->num_operands - 1;
    int64_t total = *offset;
    for (size_t position = 1; position < last; position++)
    {
        anvil_gep_step_t step;
        int64_t displacement;
        if (!anvil_gep_analyze_step(&current, instr->operands[position], position - 1, &step) ||
            !anvil_gep_const_step_offset(&step, instr->operands[position], &displacement) || !anvil_gep_accumulate_offset(&total, displacement))
            return false;
    }
    const anvil_value_t *value = instr->operands[last];
    anvil_gep_step_t step;
    if (value->kind == ANVIL_VAL_CONST_INT || !anvil_gep_analyze_step(&current, value, last - 1, &step) || step.kind != ANVIL_GEP_STEP_SCALE ||
        !step.amount || step.amount > (size_t)INT64_MAX)
        return false;
    size_t pointer_size = value->owner_ctx ? value->owner_ctx->data_layout.pointer.size : 8;
    int64_t amount;
    while (value->kind == ANVIL_VAL_INSTR && anvil_memory_index_term(value->data.instr, step.amount, pointer_size, &amount) &&
           anvil_gep_accumulate_offset(&total, amount * (int64_t)step.amount))
        value = value->data.instr->operands[0];
    *offset = total;
    *index = value;
    *scale = step.amount;
    return true;
}

/* `peel`: a GEP with one variable index records it (variable_index)
 * instead of becoming the root. */
static bool describe_location(const anvil_value_t *pointer, size_t size, bool peel, memory_location_t *location)
{
    if (!pointer || !size)
        return false;

    *location = (memory_location_t){ .base = pointer, .size = size };
    for (;;)
    {
        const anvil_value_t *base = location->base;
        bool constant = base->kind == ANVIL_VAL_CONST_GEP || base->kind == ANVIL_VAL_CONST_SYMBOL_ADDR;
        if (constant && !base->data.reloc.symbol)
            return false;
        const anvil_value_t *source = address_source(base);
        if (!source)
            break;
        if (constant)
        {
            if (!anvil_gep_accumulate_offset(&location->offset, base->data.reloc.addend))
                return false;
        }
        else if (base->data.instr->op != ANVIL_OP_BITCAST)
        {
            /* A variable index: this address is the root, constant offsets
             * from it still apart; or, peeled, the index joins the location. */
            int64_t offset = location->offset;
            if (!add_gep_offset(base->data.instr, &offset) &&
                (!peel || location->index || !variable_index(base->data.instr, &offset, &location->index, &location->scale)))
                break;
            location->offset = offset;
        }
        location->base = source;
    }

    const anvil_value_t *base = location->base;
    location->local = base->kind == ANVIL_VAL_INSTR && base->data.instr->op == ANVIL_OP_ALLOCA && !base->data.instr->num_operands;
    if (location->index)
        return true;
    anvil_type_t *object = anvil_sem_memory_object_type(base);
    const anvil_arch_info_t *architecture = anvil_ctx_get_arch_info(pointer->owner_ctx);
    uint64_t maximum_object = architecture && architecture->addr_bits > 1 && architecture->addr_bits <= 64 ?
                              (UINT64_MAX >> (65 - architecture->addr_bits)) : 0;
    if ((location->local || base->kind == ANVIL_VAL_GLOBAL) && object && location->offset >= 0 &&
        object->size <= maximum_object && (uint64_t)location->offset <= object->size && size <= object->size - (size_t)location->offset)
        location->bounded = true;

    return true;
}

bool anvil_memory_offset(const anvil_value_t *pointer, const anvil_value_t **root, int64_t *offset)
{
    memory_location_t location;
    if (!root || !offset || !describe_location(pointer, 1, false, &location))
        return false;
    *root = location.base;
    *offset = location.offset;
    return true;
}

bool anvil_memory_read_only(const anvil_value_t *pointer)
{
    for (const anvil_value_t *source; pointer && (source = address_source(pointer));)
        pointer = source;
    return anvil_global_is_constant(pointer);
}

bool anvil_memory_bounded_range(const anvil_value_t *pointer, size_t size, const anvil_value_t **object, size_t *offset)
{
    memory_location_t location;
    if (!object || !offset || !describe_location(pointer, size, false, &location) || !location.bounded)
        return false;

    *object = location.base;
    *offset = (size_t)location.offset;
    return true;
}

anvil_alias_result_t anvil_memory_alias(const anvil_value_t *left, size_t left_size, const anvil_value_t *right, size_t right_size)
{
    if (left && left == right && left_size && left_size == right_size)
        return ANVIL_ALIAS_MUST;

    memory_location_t first;
    memory_location_t second;
    if (!describe_location(left, left_size, true, &first) || !describe_location(right, right_size, true, &second))
        return ANVIL_ALIAS_MAY;

    bool same = first.base == second.base && first.index == second.index && first.scale == second.scale;
    if (same && first.offset == second.offset && first.size == second.size)
        return ANVIL_ALIAS_MUST;
    /* Constant offsets from one pointer value, or from one base and one
     * index value: the ranges decide, whatever it points into.  (One value:
     * compared where one access dominates the other, it holds the same
     * address at both.) */
    if (same) {
        int64_t first_end = first.offset + (int64_t)first.size, second_end = second.offset + (int64_t)second.size;
        return first_end <= second.offset || second_end <= first.offset ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;
    }
    if (!first.bounded || !second.bounded || first.index || second.index)
        return ANVIL_ALIAS_MAY;

    if (first.base != second.base)
        return first.local || second.local ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;

    size_t first_start = (size_t)first.offset;
    size_t second_start = (size_t)second.offset;
    if (first_start < second_start)
        return first.size <= second_start - first_start ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;

    return second.size <= first_start - second_start ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;
}
