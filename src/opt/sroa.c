/* Split nonescaping aggregates accessed through constant, disjoint addresses. */
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

typedef struct {
    anvil_value_t *pointer;
    size_t offset;
    size_t slice;
} aggregate_address_t;

typedef struct {
    anvil_type_t *type;
    size_t offset;
    anvil_instr_t *allocation;
} aggregate_slice_t;

typedef struct {
    aggregate_address_t *addresses;
    aggregate_slice_t *slices;
    size_t address_count;
    size_t slice_count;
    size_t capacity;
    size_t object_size;
} aggregate_plan_t;

static bool address_offset(const anvil_instr_t *instr, size_t base, size_t *offset)
{
    if (base > INT64_MAX)
        return false;

    int64_t total = (int64_t)base;
    if (instr->op == ANVIL_OP_STRUCT_GEP)
    {
        size_t field = (size_t)instr->operands[1]->data.u;
        if (field >= instr->aux_type->data.struc.num_fields)
            return false;

        size_t displacement = instr->aux_type->data.struc.offsets[field];
        if (displacement > INT64_MAX || !anvil_gep_accumulate_offset(&total, (int64_t)displacement))
            return false;
    }
    else
    {
        anvil_type_t *current = instr->aux_type;
        for (size_t index = 1; index < instr->num_operands; index++)
        {
            anvil_value_t *value = instr->operands[index];
            anvil_gep_step_t step;
            int64_t displacement;
            if (value->kind != ANVIL_VAL_CONST_INT || !anvil_gep_analyze_step(&current, value, index - 1, &step) ||
                !anvil_gep_const_step_offset(&step, value, &displacement) || !anvil_gep_accumulate_offset(&total, displacement))
                return false;
        }
    }

    if (total < 0 || (uint64_t)total > SIZE_MAX)
        return false;

    *offset = (size_t)total;
    return true;
}

static bool register_slice(aggregate_plan_t *plan, aggregate_address_t *address)
{
    anvil_type_t *type = address->pointer->type->data.pointee;
    if (!type || !((type->kind >= ANVIL_TYPE_I1 && type->kind <= ANVIL_TYPE_F64) || type->kind == ANVIL_TYPE_PTR) ||
        address->offset > plan->object_size || type->size > plan->object_size - address->offset)
        return false;

    for (size_t index = 0; index < plan->slice_count; index++)
    {
        aggregate_slice_t *slice = &plan->slices[index];
        if (slice->offset == address->offset && anvil_types_equal(slice->type, type))
        {
            address->slice = index;
            return true;
        }

        if (address->offset < slice->offset + slice->type->size && slice->offset < address->offset + type->size)
            return false;
    }

    if (plan->slice_count == plan->capacity)
        return false;

    address->slice = plan->slice_count;
    plan->slices[plan->slice_count++] = (aggregate_slice_t){ .type = type, .offset = address->offset };
    return true;
}

static bool collect_address_users(anvil_func_t *func, aggregate_plan_t *plan, size_t address_index)
{
    aggregate_address_t *address = &plan->addresses[address_index];
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            for (size_t operand = 0; operand < instr->num_operands; operand++)
            {
                if (instr->operands[operand] != address->pointer)
                    continue;

                if ((instr->op == ANVIL_OP_GEP || instr->op == ANVIL_OP_STRUCT_GEP) && operand == 0)
                {
                    size_t offset;
                    if (plan->address_count == plan->capacity || !address_offset(instr, address->offset, &offset) || offset > plan->object_size)
                        return false;

                    plan->addresses[plan->address_count++] = (aggregate_address_t){ .pointer = instr->result, .offset = offset, .slice = SIZE_MAX };
                }
                else if ((instr->op == ANVIL_OP_LOAD && operand == 0) || (instr->op == ANVIL_OP_STORE && operand == 1))
                {
                    if (instr->memory_access.is_volatile || !register_slice(plan, address))
                        return false;
                }
                else
                {
                    /* Address identity, escapes, bulk copies and reinterpretation
                     * may observe layout or aliasing that splitting would change. */
                    return false;
                }
            }
        }
    }

    return true;
}

static bool prepare_slices(anvil_func_t *func, aggregate_plan_t *plan)
{
    anvil_ctx_t *ctx = func->parent->ctx;
    for (size_t index = 0; index < plan->slice_count; index++)
    {
        aggregate_slice_t *slice = &plan->slices[index];
        anvil_type_t *pointer = anvil_type_ptr(ctx, slice->type);
        if (!pointer)
            return false;

        slice->allocation = anvil_instr_create(ctx, ANVIL_OP_ALLOCA, pointer, "scalarized");
        if (!slice->allocation)
            return false;
    }

    return true;
}

static void apply_slices(anvil_func_t *func, anvil_instr_t *allocation, aggregate_plan_t *plan)
{
    anvil_block_t *entry = allocation->parent;
    for (size_t index = 0; index < plan->slice_count; index++)
    {
        anvil_instr_t *scalar = plan->slices[index].allocation;
        scalar->parent = entry;
        scalar->owner_module = func->parent;
        scalar->result->owner_module = func->parent;
        scalar->prev = allocation->prev;
        scalar->next = allocation;
        if (allocation->prev)
            allocation->prev->next = scalar;
        else
            entry->first = scalar;

        allocation->prev = scalar;
    }

    /* Rewrite scalar addresses before unlinking the entire derived-address
     * tree. No allocation or fallible operation remains after this point. */
    for (size_t index = 0; index < plan->address_count; index++)
    {
        aggregate_address_t *address = &plan->addresses[index];
        if (address->slice != SIZE_MAX)
            anvil_opt_replace_uses_in_func(func, address->pointer, plan->slices[address->slice].allocation->result);
    }

    for (size_t index = plan->address_count; index > 0; index--)
        anvil_opt_erase_instr(plan->addresses[index - 1].pointer->data.instr);
}

anvil_pass_result_t anvil_pass_sroa(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->entry)
        return ANVIL_PASS_RUN_UNCHANGED;

    size_t capacity = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            if (capacity == SIZE_MAX)
            {
                anvil_set_error(ctx, ANVIL_ERR_NOMEM, "Aggregate analysis size overflow");
                return ANVIL_PASS_RUN_ERROR;
            }

            capacity++;
        }
    }

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (anvil_instr_t *instr = func->entry->first; instr; instr = instr->next)
    {
        if (instr->op != ANVIL_OP_ALLOCA || instr->num_operands)
            continue;

        anvil_type_t *type = instr->result->type->data.pointee;
        if (type->kind != ANVIL_TYPE_STRUCT && type->kind != ANVIL_TYPE_ARRAY)
            continue;

        aggregate_plan_t plan = { .capacity = capacity, .object_size = type->size };
        plan.addresses = anvil_ctx_calloc(ctx, capacity, sizeof(*plan.addresses));
        plan.slices = anvil_ctx_calloc(ctx, capacity, sizeof(*plan.slices));
        if (!plan.addresses || !plan.slices)
        {
            free(plan.addresses);
            free(plan.slices);
            return ANVIL_PASS_RUN_ERROR;
        }

        plan.addresses[plan.address_count++] = (aggregate_address_t){ .pointer = instr->result, .slice = SIZE_MAX };
        bool eligible = true;
        for (size_t index = 0; eligible && index < plan.address_count; index++)
            eligible = collect_address_users(func, &plan, index);

        if (eligible)
        {
            if (prepare_slices(func, &plan))
            {
                apply_slices(func, instr, &plan);
                result = ANVIL_PASS_RUN_CHANGED;
            }
            else
            {
                result = ANVIL_PASS_RUN_ERROR;
            }
        }

        free(plan.addresses);
        free(plan.slices);
        if (result == ANVIL_PASS_RUN_ERROR)
            break;
    }

    return result;
}
