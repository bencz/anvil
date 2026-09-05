#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdio.h>
#include <stdlib.h>

/* Small leaf bodies only. Stack/varargs operations require a separate lifetime
 * and ABI policy; excluding nested calls also prevents recursive expansion. */
enum { INLINE_BODY_LIMIT = 48, INLINE_CALLER_LIMIT = 1024 };

typedef struct {
    anvil_opt_cfg_t cfg;
    anvil_block_t **blocks;
    anvil_instr_t **originals;
    anvil_instr_t **copies;
    size_t instruction_count;
    anvil_instr_t *call;
} inline_plan;

static anvil_func_t *direct_callee(anvil_instr_t *call)
{
    if (call->op != ANVIL_OP_CALL || !call->num_operands)
        return NULL;

    anvil_value_t *value = call->operands[0];
    if (value->kind == ANVIL_VAL_CONST_SYMBOL_ADDR)
        value = value->data.reloc.symbol;

    return value && value->kind == ANVIL_VAL_FUNC ? value->data.func : NULL;
}

static size_t body_cost(const anvil_func_t *callee)
{
    if (callee->linkage != ANVIL_LINK_INTERNAL || callee->is_declaration || callee->type->data.func.variadic)
        return SIZE_MAX;

    size_t cost = 0;
    size_t returns = 0;
    for (anvil_block_t *block = callee->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            if (instr->op == ANVIL_OP_CALL || instr->op == ANVIL_OP_ALLOCA || instr->op == ANVIL_OP_VA_START)
                return SIZE_MAX;

            if (instr->true_block == callee->entry || instr->false_block == callee->entry)
                return SIZE_MAX;

            for (size_t index = 0; index < instr->num_switch_cases; index++)
            {
                if (instr->switch_blocks[index] == callee->entry)
                    return SIZE_MAX;
            }

            returns += instr->op == ANVIL_OP_RET;
            cost += instr->op == ANVIL_OP_SWITCH ? 1 + instr->num_switch_cases : 1;
            if (cost > INLINE_BODY_LIMIT)
                return SIZE_MAX;
        }
    }

    return returns ? cost : SIZE_MAX;
}

static anvil_value_t *mapped_value(const inline_plan *plan, anvil_value_t *value)
{
    if (value->kind == ANVIL_VAL_PARAM)
        return plan->call->operands[value->data.param.index + 1];

    if (value->kind != ANVIL_VAL_INSTR)
        return value;

    for (size_t index = 0; index < plan->instruction_count; index++)
    {
        if (plan->originals[index] == value->data.instr)
            return plan->copies[index]->result;
    }

    return NULL;
}

static anvil_block_t *mapped_block(const inline_plan *plan, anvil_block_t *block)
{
    if (!block)
        return NULL;

    size_t index = anvil_opt_cfg_index(&plan->cfg, block);
    return index == SIZE_MAX ? NULL : plan->blocks[index];
}

static void destroy_plan(inline_plan *plan)
{
    anvil_opt_cfg_destroy(&plan->cfg);
    free(plan->blocks);
    free(plan->originals);
    free(plan->copies);
}

static anvil_block_t *prepare_block(anvil_func_t *caller)
{
    char name[64];
    snprintf(name, sizeof(name), "inline.%u", caller->owner_ctx->next_block_id);
    return anvil_block_prepare(caller, name);
}

static bool prepare_body(inline_plan *plan, anvil_func_t *caller, anvil_func_t *callee)
{
    anvil_ctx_t *ctx = caller->owner_ctx;
    if (!anvil_opt_cfg_build(callee, &plan->cfg))
        return false;

    plan->blocks = anvil_ctx_calloc(ctx, plan->cfg.count, sizeof(*plan->blocks));
    plan->originals = anvil_ctx_calloc(ctx, INLINE_BODY_LIMIT, sizeof(*plan->originals));
    plan->copies = anvil_ctx_calloc(ctx, INLINE_BODY_LIMIT, sizeof(*plan->copies));
    if (!plan->blocks || !plan->originals || !plan->copies)
        return false;

    for (size_t block = 0; block < plan->cfg.count; block++)
    {
        plan->blocks[block] = prepare_block(caller);
        if (!plan->blocks[block])
            return false;

        for (anvil_instr_t *original = plan->cfg.blocks[block]->first; original; original = original->next)
        {
            anvil_type_t *type = original->result ? original->result->type : ctx->type_void;
            anvil_op_t op = original->op == ANVIL_OP_RET ? ANVIL_OP_BR : original->op;
            anvil_instr_t *copy = anvil_instr_create(ctx, op, type, original->result ? original->result->name : NULL);
            if (!copy)
                return false;

            size_t index = plan->instruction_count++;
            plan->originals[index] = original;
            plan->copies[index] = copy;
            copy->fcmp_pred = original->fcmp_pred;
            copy->call_cc = original->call_cc;
            copy->memory_access = original->memory_access;
            copy->atomic = original->atomic;
            copy->aux_type = original->aux_type;
            anvil_block_append_prepared(plan->blocks[block], copy);
        }
    }

    return true;
}

static bool wire_body(inline_plan *plan, anvil_block_t *continuation, anvil_instr_t *result)
{
    anvil_ctx_t *ctx = continuation->parent->owner_ctx;
    for (size_t index = 0; index < plan->instruction_count; index++)
    {
        anvil_instr_t *original = plan->originals[index];
        anvil_instr_t *copy = plan->copies[index];
        if (original->op == ANVIL_OP_RET)
        {
            copy->true_block = continuation;
            if (result)
            {
                size_t incoming = result->num_phi_incoming++;
                result->phi_blocks[incoming] = copy->parent;
                result->operands[incoming] = mapped_value(plan, original->operands[0]);
                result->num_operands++;
            }
            continue;
        }

        if (!anvil_instr_reserve_operands(copy, original->num_operands))
            return false;

        for (size_t operand = 0; operand < original->num_operands; operand++)
        {
            copy->operands[operand] = mapped_value(plan, original->operands[operand]);
            if (!copy->operands[operand])
                return false;
        }
        copy->num_operands = original->num_operands;
        copy->true_block = mapped_block(plan, original->true_block);
        copy->false_block = mapped_block(plan, original->false_block);

        if (original->num_phi_incoming)
        {
            copy->phi_blocks = anvil_ctx_calloc(ctx, original->num_phi_incoming, sizeof(*copy->phi_blocks));
            if (!copy->phi_blocks)
                return false;

            copy->num_phi_incoming = original->num_phi_incoming;
            copy->phi_capacity = original->num_phi_incoming;
            for (size_t incoming = 0; incoming < copy->num_phi_incoming; incoming++)
                copy->phi_blocks[incoming] = mapped_block(plan, original->phi_blocks[incoming]);
        }

        if (original->num_switch_cases)
        {
            copy->switch_blocks = anvil_ctx_calloc(ctx, original->num_switch_cases, sizeof(*copy->switch_blocks));
            if (!copy->switch_blocks)
                return false;

            copy->num_switch_cases = original->num_switch_cases;
            copy->switch_capacity = original->num_switch_cases;
            for (size_t entry = 0; entry < copy->num_switch_cases; entry++)
                copy->switch_blocks[entry] = mapped_block(plan, original->switch_blocks[entry]);
        }
    }

    return true;
}

static bool inline_call(anvil_func_t *caller, anvil_func_t *callee, anvil_instr_t *call)
{
    inline_plan plan = { .call = call };
    anvil_ctx_t *ctx = caller->owner_ctx;
    bool success = false;
    if (!prepare_body(&plan, caller, callee))
        goto done;

    anvil_block_t *continuation = prepare_block(caller);
    anvil_instr_t *branch = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
    if (!continuation || !branch)
        goto done;

    anvil_instr_t *result = NULL;
    if (call->result)
    {
        result = anvil_instr_create(ctx, ANVIL_OP_PHI, call->result->type, "inline.result");
        if (!result || !anvil_instr_reserve_operands(result, plan.cfg.count))
            goto done;

        result->phi_blocks = anvil_ctx_calloc(ctx, plan.cfg.count, sizeof(*result->phi_blocks));
        if (!result->phi_blocks)
            goto done;

        result->phi_capacity = plan.cfg.count;
        anvil_block_append_prepared(continuation, result);
    }

    if (!wire_body(&plan, continuation, result))
        goto done;

    /* Commit only after every block, operand array and return join exists. */
    anvil_block_t *original = call->parent;
    anvil_instr_t *tail = call->next;
    anvil_instr_t *last = original->last;
    if (result)
        result->next = tail;
    else
        continuation->first = tail;

    tail->prev = result;
    continuation->last = last;
    for (anvil_instr_t *instr = tail; instr; instr = instr->next)
        instr->parent = continuation;

    original->last = call;
    call->next = NULL;
    anvil_opt_erase_instr(call);
    branch->true_block = plan.blocks[plan.cfg.entry];
    anvil_block_append_prepared(original, branch);

    for (anvil_block_t *block = caller->blocks; block; block = block->next)
    {
        for (anvil_instr_t *phi = block->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next)
        {
            for (size_t incoming = 0; incoming < phi->num_phi_incoming; incoming++)
            {
                if (phi->phi_blocks[incoming] == original)
                    phi->phi_blocks[incoming] = continuation;
            }
        }
    }

    for (size_t block = 0; block < plan.cfg.count; block++)
        anvil_func_append_block(caller, plan.blocks[block]);

    anvil_func_append_block(caller, continuation);
    if (result)
        anvil_opt_replace_uses_in_func(caller, call->result, result->result);

    anvil_func_invalidate_cfg(caller);
    success = true;

done:
    destroy_plan(&plan);
    return success;
}

anvil_pass_result_t anvil_pass_inline(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_clear_error(func->owner_ctx);
    size_t size = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
            size++;
    }

    bool changed = false;
    for (anvil_block_t *block = func->blocks; block && size < INLINE_CALLER_LIMIT; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            anvil_func_t *callee = direct_callee(instr);
            if (!callee || callee == func || callee->parent != func->parent || callee->fp_vectorization != func->fp_vectorization || !instr->next)
                continue;

            size_t cost = body_cost(callee);
            if (cost > INLINE_BODY_LIMIT || cost + 2 > INLINE_CALLER_LIMIT - size)
                continue;

            if (!inline_call(func, callee, instr))
                return ANVIL_PASS_RUN_ERROR;

            changed = true;
            size += cost + 2;
            break;
        }
    }

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
