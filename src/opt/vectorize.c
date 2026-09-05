#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

enum { VECTOR_LANES = 4, VECTOR_SEARCH_LIMIT = 128 };

typedef struct {
    anvil_instr_t *stores[VECTOR_LANES];
    anvil_instr_t *operations[VECTOR_LANES];
    anvil_instr_t *loads[VECTOR_LANES * 2];
    anvil_type_t *element;
    anvil_op_t operation;
    size_t lanes;
} vector_group;

static bool single_use(const anvil_def_use_t *uses, const anvil_value_t *value)
{
    size_t definition = anvil_def_use_definition(uses, value);
    return definition != SIZE_MAX && uses->use_offsets[definition + 1] - uses->use_offsets[definition] == 1;
}

static bool selected_instruction(anvil_instr_t *instruction, anvil_instr_t *const *selected, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        if (instruction == selected[index])
            return true;
    }

    return false;
}

static bool collect_lane(vector_group *group, const anvil_def_use_t *uses, anvil_instr_t *store, size_t lane)
{
    if (store->memory_access.is_volatile || store->operands[0]->kind != ANVIL_VAL_INSTR)
        return false;

    anvil_instr_t *operation = store->operands[0]->data.instr;
    if (operation->parent != store->parent || operation->num_operands != 2 || !single_use(uses, operation->result))
        return false;

    if (!lane)
    {
        if (operation->op != ANVIL_OP_FADD && operation->op != ANVIL_OP_FSUB && operation->op != ANVIL_OP_FMUL && operation->op != ANVIL_OP_FDIV)
            return false;

        group->element = operation->result->type;
        if (!anvil_type_is_floating(group->element))
            return false;

        group->lanes = 16 / group->element->size;
        group->operation = operation->op;
    }
    else if (operation->op != group->operation || !anvil_types_equal(operation->result->type, group->element))
        return false;

    for (size_t operand = 0; operand < 2; operand++)
    {
        anvil_value_t *value = operation->operands[operand];
        if (value->kind != ANVIL_VAL_INSTR || !single_use(uses, value))
            return false;

        anvil_instr_t *load = value->data.instr;
        if (load->parent != store->parent || load->op != ANVIL_OP_LOAD || load->memory_access.is_volatile)
            return false;

        group->loads[lane * 2 + operand] = load;
    }

    group->stores[lane] = store;
    group->operations[lane] = operation;
    return true;
}

static anvil_value_t *lane_address(const vector_group *group, size_t lane, size_t stream)
{
    return stream == 2 ? group->stores[lane]->operands[1] : group->loads[lane * 2 + stream]->operands[0];
}

static bool contiguous_ranges(const vector_group *group)
{
    for (size_t stream = 0; stream < 3; stream++)
    {
        const anvil_value_t *base;
        size_t offset;
        if (!anvil_memory_bounded_range(lane_address(group, 0, stream), 16, &base, &offset))
            return false;

        for (size_t lane = 1; lane < group->lanes; lane++)
        {
            const anvil_value_t *next_base;
            size_t next_offset;
            if (!anvil_memory_bounded_range(lane_address(group, lane, stream), group->element->size, &next_base, &next_offset) ||
                next_base != base || next_offset != offset + lane * group->element->size)
                return false;
        }
    }

    for (size_t stream = 0; stream < 2; stream++)
    {
        if (anvil_memory_alias(lane_address(group, 0, stream), 16, lane_address(group, 0, 2), 16) == ANVIL_ALIAS_MAY)
            return false;
    }

    return true;
}

static bool safe_span(const vector_group *group)
{
    bool started = false;
    size_t length = 0;
    for (anvil_instr_t *instruction = group->stores[0]->parent->first; instruction; instruction = instruction->next)
    {
        bool load = selected_instruction(instruction, group->loads, group->lanes * 2);
        started |= load;
        if (!started)
            continue;
        if (++length > VECTOR_SEARCH_LIMIT)
            return false;

        if (load || selected_instruction(instruction, group->operations, group->lanes) || selected_instruction(instruction, group->stores, group->lanes))
        {
            if (instruction == group->stores[group->lanes - 1])
                return true;

            continue;
        }

        switch (instruction->op)
        {
            case ANVIL_OP_GEP:
            case ANVIL_OP_STRUCT_GEP:
            case ANVIL_OP_BITCAST:
            case ANVIL_OP_ADD:
            case ANVIL_OP_SUB:
            case ANVIL_OP_MUL:
            case ANVIL_OP_TRUNC:
            case ANVIL_OP_ZEXT:
            case ANVIL_OP_SEXT:
            case ANVIL_OP_NOP:
                break;
            default:
                return false;
        }
    }

    return false;
}

static anvil_instr_t *prepare_instruction(anvil_block_t *block, anvil_op_t operation, anvil_type_t *type, anvil_value_t *left, anvil_value_t *right)
{
    anvil_ctx_t *ctx = block->parent->owner_ctx;
    anvil_instr_t *instruction = anvil_instr_create(ctx, operation, type, "vectorized");
    anvil_value_t *operands[] = { left, right };
    if (!instruction || !anvil_instr_add_operands(instruction, operands, right ? 2 : 1))
        return NULL;

    anvil_block_append_prepared(block, instruction);
    return instruction;
}

static anvil_pass_result_t replace_group(anvil_func_t *func, const vector_group *group)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_type_t *vector = anvil_type_vector(ctx, group->element, group->lanes);
    if (!vector)
        return ANVIL_PASS_RUN_ERROR;

    unsigned arithmetic = anvil_vector_operation_cost(ctx, group->operation, vector);
    unsigned load = anvil_vector_operation_cost(ctx, ANVIL_OP_LOAD, vector);
    unsigned store = anvil_vector_operation_cost(ctx, ANVIL_OP_STORE, vector);
    if (!arithmetic || !load || !store || arithmetic + 2 * load + store >= group->lanes * (arithmetic + 3))
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_type_t *pointer = anvil_type_ptr(ctx, vector);
    anvil_block_t *prepared = anvil_block_prepare(func, "vector.prepared");
    if (!pointer || !prepared)
        return ANVIL_PASS_RUN_ERROR;

    anvil_instr_t *addresses[3];
    for (size_t stream = 0; stream < 3; stream++)
    {
        addresses[stream] = prepare_instruction(prepared, ANVIL_OP_BITCAST, pointer, lane_address(group, 0, stream), NULL);
        if (!addresses[stream])
            return ANVIL_PASS_RUN_ERROR;
    }

    anvil_instr_t *left = prepare_instruction(prepared, ANVIL_OP_LOAD, vector, addresses[0]->result, NULL);
    anvil_instr_t *right = prepare_instruction(prepared, ANVIL_OP_LOAD, vector, addresses[1]->result, NULL);
    if (!left || !right)
        return ANVIL_PASS_RUN_ERROR;

    anvil_instr_t *result = prepare_instruction(prepared, group->operation, vector, left->result, right->result);
    if (!result || !prepare_instruction(prepared, ANVIL_OP_STORE, ctx->type_void, result->result, addresses[2]->result))
        return ANVIL_PASS_RUN_ERROR;

    anvil_instr_t *anchor = group->stores[group->lanes - 1];
    anvil_block_t *block = anchor->parent;
    prepared->first->prev = anchor->prev;
    if (anchor->prev)
        anchor->prev->next = prepared->first;
    else
        block->first = prepared->first;

    prepared->last->next = anchor;
    anchor->prev = prepared->last;
    for (anvil_instr_t *instruction = prepared->first; instruction != anchor; instruction = instruction->next)
        instruction->parent = block;

    prepared->first = NULL;
    prepared->last = NULL;
    for (size_t lane = 0; lane < group->lanes; lane++)
    {
        anvil_opt_erase_instr(group->stores[lane]);
        anvil_opt_erase_instr(group->operations[lane]);
        anvil_opt_erase_instr(group->loads[lane * 2]);
        anvil_opt_erase_instr(group->loads[lane * 2 + 1]);
    }

    return ANVIL_PASS_RUN_CHANGED;
}

anvil_pass_result_t anvil_pass_vectorize(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    if (!func->fp_vectorization || !func->owner_ctx->backend->ops->vector_operation_cost)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_def_use_t uses;
    if (!anvil_def_use_build(func, &uses))
        return ANVIL_PASS_RUN_ERROR;

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instruction = block->first; instruction; instruction = instruction->next)
        {
            if (instruction->op != ANVIL_OP_STORE || !instruction->parent)
                continue;

            vector_group group = { 0 };
            if (!collect_lane(&group, &uses, instruction, 0))
                continue;

            size_t lane = 1;
            size_t examined = 0;
            for (anvil_instr_t *next = instruction->next; next && lane < group.lanes && examined++ < VECTOR_SEARCH_LIMIT; next = next->next)
            {
                if (next->op == ANVIL_OP_STORE)
                {
                    if (!collect_lane(&group, &uses, next, lane))
                        break;

                    lane++;
                }
            }

            if (lane != group.lanes || !contiguous_ranges(&group) || !safe_span(&group))
                continue;

            anvil_pass_result_t status = replace_group(func, &group);
            if (status == ANVIL_PASS_RUN_ERROR)
            {
                anvil_def_use_destroy(&uses);
                return status;
            }
            if (status == ANVIL_PASS_RUN_CHANGED)
            {
                result = status;
                instruction = group.stores[group.lanes - 1];
            }
        }
    }

    anvil_def_use_destroy(&uses);
    return result;
}
