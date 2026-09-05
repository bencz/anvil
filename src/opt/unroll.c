#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

enum { UNROLL_ITERATION_LIMIT = 8, UNROLL_BODY_LIMIT = 32, UNROLL_GROWTH_LIMIT = 128 };

typedef struct {
    anvil_block_t *header;
    anvil_block_t *body;
    anvil_block_t *exit;
    anvil_instr_t *phis[UNROLL_BODY_LIMIT];
    anvil_value_t *initial[UNROLL_BODY_LIMIT];
    anvil_value_t *backedge[UNROLL_BODY_LIMIT];
    anvil_instr_t *instructions[UNROLL_BODY_LIMIT];
    anvil_value_t *condition;
    anvil_value_t *condition_true;
    size_t phi_count;
    size_t instruction_count;
    size_t iterations;
} unroll_plan;

static bool compare_induction(anvil_op_t operation, uint64_t value, uint64_t limit, unsigned bits, bool *result)
{
    if (operation == ANVIL_OP_CMP_LT || operation == ANVIL_OP_CMP_LE || operation == ANVIL_OP_CMP_GT || operation == ANVIL_OP_CMP_GE)
    {
        uint64_t sign = UINT64_C(1) << (bits - 1);
        value ^= sign;
        limit ^= sign;
    }

    switch (operation)
    {
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_ULT:
            *result = value < limit;
            return true;
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_ULE:
            *result = value <= limit;
            return true;
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_UGT:
            *result = value > limit;
            return true;
        case ANVIL_OP_CMP_GE:
        case ANVIL_OP_CMP_UGE:
            *result = value >= limit;
            return true;
        case ANVIL_OP_CMP_NE:
            *result = value != limit;
            return true;
        case ANVIL_OP_CMP_EQ:
            *result = value == limit;
            return true;
        default:
            return false;
    }
}

static bool trip_count(unroll_plan *plan, anvil_instr_t *comparison)
{
    if (comparison->num_operands != 2 || !anvil_opt_is_const_int(comparison->operands[1]))
        return false;

    size_t induction = 0;
    while (induction < plan->phi_count && plan->phis[induction]->result != comparison->operands[0])
        induction++;

    if (induction == plan->phi_count || !anvil_opt_is_const_int(plan->initial[induction]))
        return false;

    anvil_value_t *updated = plan->backedge[induction];
    if (updated->kind != ANVIL_VAL_INSTR)
        return false;

    anvil_instr_t *step = updated->data.instr;
    if (step->parent != plan->body || (step->op != ANVIL_OP_ADD && step->op != ANVIL_OP_SUB) ||
        step->operands[0] != comparison->operands[0] || !anvil_opt_is_const_int(step->operands[1]))
        return false;

    unsigned bits = (unsigned)comparison->operands[0]->type->size * 8;
    if (!bits || bits > 64 || comparison->operands[0]->type->kind == ANVIL_TYPE_I1)
        return false;

    uint64_t mask = UINT64_MAX >> (64 - bits);
    uint64_t value = (uint64_t)plan->initial[induction]->data.i & mask;
    uint64_t limit = (uint64_t)comparison->operands[1]->data.i & mask;
    uint64_t increment = (uint64_t)step->operands[1]->data.i;
    if (step->op == ANVIL_OP_SUB)
        increment = UINT64_C(0) - increment;

    for (size_t iteration = 0; iteration <= UNROLL_ITERATION_LIMIT; iteration++)
    {
        bool again;
        if (!compare_induction(comparison->op, value, limit, bits, &again))
            return false;

        if (!again)
        {
            plan->iterations = iteration;
            return true;
        }

        value = (value + increment) & mask;
    }

    return false;
}

static bool recognize_loop(const anvil_opt_cfg_t *cfg, size_t header_index, unroll_plan *plan)
{
    anvil_block_t *header = cfg->blocks[header_index];
    anvil_instr_t *branch = header->last;
    if (!branch || branch->op != ANVIL_OP_BR_COND || branch->true_block == branch->false_block ||
        branch->true_block == header || branch->false_block == header)
        return false;

    anvil_block_t *body = branch->true_block;
    if (!body->last || body->last->op != ANVIL_OP_BR || body->last->true_block != header)
        return false;

    size_t body_index = anvil_opt_cfg_index(cfg, body);
    if (body_index == SIZE_MAX || cfg->predecessor_offsets[body_index + 1] - cfg->predecessor_offsets[body_index] != 1 ||
        cfg->predecessor_offsets[header_index + 1] - cfg->predecessor_offsets[header_index] != 2 ||
        !anvil_opt_cfg_dominates(cfg, header_index, body_index))
        return false;

    plan->header = header;
    plan->body = body;
    plan->exit = branch->false_block;
    anvil_instr_t *comparison = header->first;
    while (comparison && comparison->op == ANVIL_OP_PHI)
    {
        if (plan->phi_count == UNROLL_BODY_LIMIT || comparison->num_phi_incoming != 2)
            return false;

        size_t back = comparison->phi_blocks[0] == body ? 0 : 1;
        if (comparison->phi_blocks[back] != body)
            return false;

        size_t index = plan->phi_count++;
        plan->phis[index] = comparison;
        plan->initial[index] = comparison->operands[1 - back];
        plan->backedge[index] = comparison->operands[back];
        comparison = comparison->next;
    }

    if (!comparison || comparison->next != branch || branch->operands[0] != comparison->result || !trip_count(plan, comparison))
        return false;

    plan->condition = comparison->result;

    for (anvil_instr_t *instruction = body->first; instruction != body->last; instruction = instruction->next)
    {
        if (plan->instruction_count == UNROLL_BODY_LIMIT || instruction->op == ANVIL_OP_PHI ||
            instruction->op == ANVIL_OP_ALLOCA || instruction->op == ANVIL_OP_VA_START)
            return false;

        plan->instructions[plan->instruction_count++] = instruction;
    }

    return plan->instruction_count * plan->iterations <= UNROLL_GROWTH_LIMIT;
}

static anvil_value_t *map_value(const unroll_plan *plan, anvil_value_t **values, anvil_instr_t **copies, size_t copied, anvil_value_t *value)
{
    if (value == plan->condition)
        return plan->condition_true;

    for (size_t index = 0; index < plan->phi_count; index++)
    {
        if (value == plan->phis[index]->result)
            return values[index];
    }

    for (size_t index = 0; index < copied; index++)
    {
        if (value == plan->instructions[index]->result)
            return copies[index]->result;
    }

    return value;
}

static bool expand_loop(anvil_func_t *func, unroll_plan *plan)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_block_t *prepared = anvil_block_prepare(func, "unroll.prepared");
    if (!prepared)
        return false;

    plan->condition_true = anvil_const_i1(ctx, true);
    anvil_value_t *condition_false = anvil_const_i1(ctx, false);
    if (!plan->condition_true || !condition_false)
        return false;

    anvil_value_t *values[UNROLL_BODY_LIMIT];
    for (size_t index = 0; index < plan->phi_count; index++)
        values[index] = plan->initial[index];

    for (size_t iteration = 0; iteration < plan->iterations; iteration++)
    {
        anvil_instr_t *copies[UNROLL_BODY_LIMIT];
        for (size_t index = 0; index < plan->instruction_count; index++)
        {
            anvil_instr_t *original = plan->instructions[index];
            anvil_type_t *type = original->result ? original->result->type : ctx->type_void;
            anvil_instr_t *copy = anvil_instr_create(ctx, original->op, type, original->result ? original->result->name : NULL);
            if (!copy || !anvil_instr_reserve_operands(copy, original->num_operands))
                return false;

            copies[index] = copy;
            copy->fcmp_pred = original->fcmp_pred;
            copy->call_cc = original->call_cc;
            copy->memory_access = original->memory_access;
            copy->atomic = original->atomic;
            copy->aux_type = original->aux_type;
            copy->num_operands = original->num_operands;
            for (size_t operand = 0; operand < original->num_operands; operand++)
                copy->operands[operand] = map_value(plan, values, copies, index, original->operands[operand]);

            anvil_block_append_prepared(prepared, copy);
        }

        anvil_value_t *next[UNROLL_BODY_LIMIT];
        for (size_t index = 0; index < plan->phi_count; index++)
            next[index] = map_value(plan, values, copies, plan->instruction_count, plan->backedge[index]);

        for (size_t index = 0; index < plan->phi_count; index++)
            values[index] = next[index];
    }

    anvil_instr_t *branch = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
    if (!branch)
        return false;

    branch->true_block = plan->exit;
    anvil_block_append_prepared(prepared, branch);

    /* All allocation is complete. Keep the header identity for exit PHIs. */
    for (size_t index = 0; index < plan->phi_count; index++)
        anvil_opt_replace_uses_in_func(func, plan->phis[index]->result, values[index]);

    anvil_opt_replace_uses_in_func(func, plan->condition, condition_false);

    while (plan->header->first)
        anvil_opt_erase_instr(plan->header->first);

    plan->header->first = prepared->first;
    plan->header->last = prepared->last;
    prepared->first = NULL;
    prepared->last = NULL;
    for (anvil_instr_t *instruction = plan->header->first; instruction; instruction = instruction->next)
        instruction->parent = plan->header;

    anvil_block_t **link = &func->blocks;
    anvil_block_t *previous = NULL;
    while (*link != plan->body)
    {
        previous = *link;
        link = &previous->next;
    }

    *link = plan->body->next;
    if (func->last_block == plan->body)
        func->last_block = previous;

    func->num_blocks--;
    anvil_func_invalidate_cfg(func);
    return true;
}

anvil_pass_result_t anvil_pass_unroll(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_clear_error(func->owner_ctx);
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (;;)
    {
        anvil_opt_cfg_t cfg;
        if (!anvil_opt_cfg_build(func, &cfg))
            return ANVIL_PASS_RUN_ERROR;

        bool changed = false;
        for (size_t index = 0; index < cfg.reachable_count; index++)
        {
            unroll_plan plan = { 0 };
            if (!recognize_loop(&cfg, cfg.rpo[index], &plan))
                continue;

            result = expand_loop(func, &plan) ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_ERROR;
            changed = true;
            break;
        }

        anvil_opt_cfg_destroy(&cfg);
        if (!changed || result == ANVIL_PASS_RUN_ERROR)
            return result;
    }
}
