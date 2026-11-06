#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdio.h>
#include <stdlib.h>

/* Callees within a size budget: at O2 small bodies, internal or external
 * ones nothing can replace (an external one stays for other callers; a weak
 * one may be replaced at link time, and under PIC a default-visibility one
 * at load time, anvil_symbol_uses_got), or larger internal ones with a
 * single call site (the copy replaces the original, which dead code removal
 * then drops); at O3 twice those.  Conversions do not count toward the
 * budget, as they mostly fold into a load, a register view or their user;
 * the caller's growth counts every instruction.  Calls inside the callee are
 * copied too, except a call of the callee itself (no recursive expansion);
 * static allocas move to the caller's entry block, so they stay static and
 * promotable wherever the call sits.  Varargs and dynamic allocas keep the
 * call.  The caller stops growing at INLINE_CALLER_LIMIT. */
enum { INLINE_SMALL_BODY = 48, INLINE_SINGLE_BODY = 64, INLINE_BODY_LIMIT = 128, INLINE_CALLER_LIMIT = 1024 };

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

/* The budget cost of `callee` (SIZE_MAX: not inlinable) and, in
 * *instructions, how many instructions a copy adds. */
static size_t body_cost(const anvil_func_t *callee, size_t *instructions)
{
    if ((callee->linkage != ANVIL_LINK_INTERNAL && callee->linkage != ANVIL_LINK_EXTERNAL) || callee->is_declaration ||
        callee->type->data.func.variadic || anvil_symbol_uses_got(callee->owner_ctx, callee->value))
        return SIZE_MAX;

    size_t cost = 0;
    *instructions = 0;
    size_t returns = 0;
    for (anvil_block_t *block = callee->blocks; block; block = block->next)
    {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next)
        {
            if (instr->op == ANVIL_OP_VA_START || (instr->op == ANVIL_OP_CALL && direct_callee(instr) == callee) ||
                (instr->op == ANVIL_OP_ALLOCA && (instr->num_operands || block != callee->entry)))
                return SIZE_MAX;

            if (instr->true_block == callee->entry || instr->false_block == callee->entry)
                return SIZE_MAX;

            for (size_t index = 0; index < instr->num_switch_cases; index++)
            {
                if (instr->switch_blocks[index] == callee->entry)
                    return SIZE_MAX;
            }

            returns += instr->op == ANVIL_OP_RET;
            bool conversion = instr->op == ANVIL_OP_TRUNC || instr->op == ANVIL_OP_ZEXT || instr->op == ANVIL_OP_SEXT || instr->op == ANVIL_OP_BITCAST;
            cost += instr->op == ANVIL_OP_SWITCH ? 1 + instr->num_switch_cases : conversion ? 0 : 1;
            if (++*instructions > INLINE_BODY_LIMIT)
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

    size_t instructions = 0;
    for (size_t block = 0; block < plan->cfg.count; block++)
        for (anvil_instr_t *original = plan->cfg.blocks[block]->first; original; original = original->next)
            instructions++;
    plan->blocks = anvil_ctx_calloc(ctx, plan->cfg.count, sizeof(*plan->blocks));
    plan->originals = anvil_ctx_calloc(ctx, instructions ? instructions : 1, sizeof(*plan->originals));
    plan->copies = anvil_ctx_calloc(ctx, instructions ? instructions : 1, sizeof(*plan->copies));
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
            if (!anvil_instr_copy_attributes(copy, original))
                return false;
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
    /* The callee's static allocas: at the top of the caller's entry block. */
    for (size_t index = 0; index < plan.instruction_count; index++)
    {
        anvil_instr_t *copy = plan.copies[index];
        if (copy->op != ANVIL_OP_ALLOCA || !caller->entry->first)
            continue;
        anvil_opt_erase_instr(copy);
        anvil_opt_insert_before(caller->entry->first, copy);
    }

    anvil_func_invalidate_cfg(caller);
    success = true;

done:
    destroy_plan(&plan);
    return success;
}

/* Inlined code keeps its own floating-point policy: the caller's must be
 * the same. */
static bool same_fp_policy(const anvil_func_t *caller, const anvil_func_t *callee)
{
    return caller->fp_vectorization == callee->fp_vectorization && caller->fp_reassociation == callee->fp_reassociation &&
           caller->fp_contraction == callee->fp_contraction;
}

anvil_pass_result_t anvil_pass_inline(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    bool aggressive = anvil_ctx_get_opt_level(func->owner_ctx) >= ANVIL_OPT_AGGRESSIVE;

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
            if (!callee || callee == func || callee->parent != func->parent || !same_fp_policy(func, callee) || !instr->next)
                continue;

            size_t instructions;
            size_t cost = body_cost(callee, &instructions);
            size_t scale = aggressive ? 2 : 1, calls = 0;
            bool fits = cost <= INLINE_SMALL_BODY * scale ||
                        (callee->linkage == ANVIL_LINK_INTERNAL && cost <= INLINE_SINGLE_BODY * scale &&
                         anvil_opt_direct_calls(callee->parent, callee, NULL, &calls) && calls == 1);
            if (!fits || instructions + 2 > INLINE_CALLER_LIMIT - size)
                continue;

            if (!inline_call(func, callee, instr))
                return ANVIL_PASS_RUN_ERROR;

            changed = true;
            size += instructions + 2;
            break;
        }
    }

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
