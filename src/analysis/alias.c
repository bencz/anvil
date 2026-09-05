#include "anvil/anvil_analysis.h"

typedef struct {
    const anvil_value_t *base;
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

static bool describe_location(const anvil_value_t *pointer, size_t size, memory_location_t *location)
{
    if (!pointer || !size)
        return false;

    *location = (memory_location_t){ .base = pointer, .size = size };
    for (;;)
    {
        const anvil_value_t *base = location->base;
        if (base->kind == ANVIL_VAL_CONST_GEP || base->kind == ANVIL_VAL_CONST_SYMBOL_ADDR)
        {
            if (!base->data.reloc.symbol || !anvil_gep_accumulate_offset(&location->offset, base->data.reloc.addend))
                return false;

            location->base = base->data.reloc.symbol;
            continue;
        }

        if (base->kind != ANVIL_VAL_INSTR)
            break;

        const anvil_instr_t *instr = base->data.instr;
        if (instr->op == ANVIL_OP_BITCAST && instr->operands[0]->type->kind == ANVIL_TYPE_PTR)
        {
            location->base = instr->operands[0];
            continue;
        }

        if (instr->op != ANVIL_OP_GEP && instr->op != ANVIL_OP_STRUCT_GEP)
            break;
        if (!add_gep_offset(instr, &location->offset))
            return false;

        location->base = instr->operands[0];
    }

    const anvil_value_t *base = location->base;
    location->local = base->kind == ANVIL_VAL_INSTR && base->data.instr->op == ANVIL_OP_ALLOCA && !base->data.instr->num_operands;
    anvil_type_t *object = anvil_sem_memory_object_type(base);
    const anvil_arch_info_t *architecture = anvil_ctx_get_arch_info(pointer->owner_ctx);
    uint64_t maximum_object = architecture && architecture->addr_bits > 1 && architecture->addr_bits <= 64 ?
                              (UINT64_MAX >> (65 - architecture->addr_bits)) : 0;
    if ((location->local || base->kind == ANVIL_VAL_GLOBAL) && object && location->offset >= 0 &&
        object->size <= maximum_object && (uint64_t)location->offset <= object->size && size <= object->size - (size_t)location->offset)
        location->bounded = true;

    return true;
}

bool anvil_memory_bounded_range(const anvil_value_t *pointer, size_t size, const anvil_value_t **object, size_t *offset)
{
    memory_location_t location;
    if (!object || !offset || !describe_location(pointer, size, &location) || !location.bounded)
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
    if (!describe_location(left, left_size, &first) || !describe_location(right, right_size, &second))
        return ANVIL_ALIAS_MAY;

    if (first.base == second.base && first.offset == second.offset && first.size == second.size)
        return ANVIL_ALIAS_MUST;
    if (!first.bounded || !second.bounded)
        return ANVIL_ALIAS_MAY;

    if (first.base != second.base)
        return first.local || second.local ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;

    size_t first_start = (size_t)first.offset;
    size_t second_start = (size_t)second.offset;
    if (first_start < second_start)
        return first.size <= second_start - first_start ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;

    return second.size <= first_start - second_start ? ANVIL_ALIAS_NO : ANVIL_ALIAS_MAY;
}
