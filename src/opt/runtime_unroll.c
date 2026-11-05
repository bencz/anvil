/*
 * ANVIL - Runtime unrolling of small counted loops (ANVIL_PASS_RUNTIME_UNROLL)
 *
 * A loop of a header and a body counting j up to zero (j != 0 tested in the
 * header, j + 1 in the body: count_to_zero's shape), with a small body and
 * no calls, gets in front of it a loop running U copies of the body per
 * iteration while j <= -U (at least U iterations left), U being the
 * target's anvil_arch_info_t.small_loop_unroll; the original loop then runs
 * what is left.  Each copy reads the loop-carried values the previous copy
 * produced; a value stepped by a constant c is v + k*c in copy k (one step
 * per iteration for the late passes: streams with offsets, the counter).
 * Loop control then costs one test and branch per U iterations.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdint.h>
#include <stdlib.h>

enum { RU_BODY_LIMIT = 16, RU_PHI_LIMIT = 8, RU_FACTOR_LIMIT = 8 };

typedef struct {
    anvil_block_t *header, *body, *preheader;
    anvil_instr_t *phis[RU_PHI_LIMIT];
    anvil_value_t *starts[RU_PHI_LIMIT], *nexts[RU_PHI_LIMIT];
    int64_t steps[RU_PHI_LIMIT];
    bool stepped[RU_PHI_LIMIT];
    size_t num_phis, counter;
    anvil_instr_t *instrs[RU_BODY_LIMIT];
    size_t count;
} ru_plan;

/* The constant c of `next` = phi + c (or phi - c); false otherwise. */
static bool constant_step(const anvil_instr_t *phi, const anvil_value_t *next, int64_t *step)
{
    if (next->kind != ANVIL_VAL_INSTR || !next->data.instr || next->data.instr->num_operands != 2)
        return false;
    const anvil_instr_t *instr = next->data.instr;
    const anvil_value_t *a = instr->operands[0], *b = instr->operands[1];
    if (instr->op == ANVIL_OP_ADD && a == phi->result && b->kind == ANVIL_VAL_CONST_INT)
        *step = b->data.i;
    else if (instr->op == ANVIL_OP_ADD && b == phi->result && a->kind == ANVIL_VAL_CONST_INT)
        *step = a->data.i;
    else if (instr->op == ANVIL_OP_SUB && a == phi->result && b->kind == ANVIL_VAL_CONST_INT && b->data.i != INT64_MIN)
        *step = -b->data.i;
    else
        return false;
    return *step != 0 && *step > -(INT64_C(1) << 40) && *step < (INT64_C(1) << 40);
}

static bool recognize(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, ru_plan *plan)
{
    const anvil_loop_info_t *info = &loops->loops[loop];
    if (info->latch_count != 1 || info->preheader == SIZE_MAX)
        return false;
    anvil_block_t *header = cfg->blocks[info->header], *body = NULL;
    size_t members = 0;
    for (size_t b = 0; b < cfg->count; b++) {
        if (!anvil_loop_contains(loops, loop, b))
            continue;
        members++;
        if (cfg->blocks[b] != header)
            body = cfg->blocks[b];
    }
    anvil_block_t *preheader = cfg->blocks[info->preheader];
    if (members != 2 || !body || header->vectorized || header->unrolled || !preheader->last || preheader->last->op != ANVIL_OP_BR ||
        preheader->last->true_block != header || !header->last || header->last->op != ANVIL_OP_BR_COND || header->last->true_block != body ||
        !body->last || body->last->op != ANVIL_OP_BR || body->last->true_block != header)
        return false;
    plan->header = header;
    plan->body = body;
    plan->preheader = preheader;

    /* Header: PHIs, the test j != 0, the branch. */
    anvil_instr_t *instr = header->first;
    plan->num_phis = 0;
    for (; instr && instr->op == ANVIL_OP_PHI; instr = instr->next) {
        if (plan->num_phis == RU_PHI_LIMIT || instr->num_phi_incoming != 2 || !instr->result)
            return false;
        size_t at = plan->num_phis++;
        plan->phis[at] = instr;
        plan->starts[at] = plan->nexts[at] = NULL;
        for (size_t item = 0; item < 2; item++) {
            if (instr->phi_blocks[item] == preheader)
                plan->starts[at] = instr->operands[item];
            else if (instr->phi_blocks[item] == body)
                plan->nexts[at] = instr->operands[item];
        }
        if (!plan->starts[at] || !plan->nexts[at])
            return false;
        plan->stepped[at] = anvil_type_is_integer(instr->result->type) && constant_step(instr, plan->nexts[at], &plan->steps[at]);
    }
    anvil_instr_t *test = instr;
    if (!test || test->next != header->last || test->op != ANVIL_OP_CMP_NE || test->num_operands != 2 || header->last->operands[0] != test->result ||
        !anvil_opt_is_zero(test->operands[1]))
        return false;
    plan->counter = SIZE_MAX;
    for (size_t p = 0; p < plan->num_phis; p++)
        if (plan->phis[p]->result == test->operands[0])
            plan->counter = p;
    if (plan->counter == SIZE_MAX || !plan->stepped[plan->counter] || plan->steps[plan->counter] != 1 ||
        !plan->phis[plan->counter]->result->type->is_signed)
        return false;

    /* Body: a few instructions, no PHI or call. */
    plan->count = 0;
    for (instr = body->first; instr && instr != body->last; instr = instr->next) {
        if (plan->count == RU_BODY_LIMIT || instr->op == ANVIL_OP_PHI || instr->op == ANVIL_OP_CALL)
            return false;
        plan->instrs[plan->count++] = instr;
    }
    return plan->count > 0;
}

static void insert_block_after(anvil_func_t *func, anvil_block_t *anchor, anvil_block_t *block)
{
    block->next = anchor->next;
    anchor->next = block;
    if (func->last_block == anchor)
        func->last_block = block;
    func->num_blocks++;
}

static anvil_instr_t *binary(anvil_ctx_t *ctx, anvil_op_t op, anvil_type_t *type, anvil_value_t *a, anvil_value_t *b, const char *name)
{
    anvil_instr_t *instr = anvil_instr_create(ctx, op, type, name);
    return instr && anvil_instr_add_operand(instr, a) && anvil_instr_add_operand(instr, b) ? instr : NULL;
}

/* `instr` at the end of the preheader. */
static anvil_value_t *before_loop(const ru_plan *plan, anvil_instr_t *instr)
{
    if (!instr)
        return NULL;
    anvil_opt_insert_before(plan->preheader->last, instr);
    return instr->result;
}

/*   preheader: trips = -j0; n = trips >> log2(U); u0 = -n;
 *              v_after = v0 + n * U * c for each value stepped by c
 *   unroll.header: u = phi [u0, preheader], [u + 1, unroll.body];
 *                  v = phi [v0, preheader], [..., unroll.body] (each value)
 *                  br u != 0, unroll.body, header
 *   unroll.body: U copies of the body; copy k reads the previous copy's
 *                values, a stepped one as v + k*c, an address indexed by it
 *                as (base + v) + k*c; then u + 1
 *   header: the remainder, entered with v_after (stepped) or v. */
static bool unroll(anvil_func_t *func, ru_plan *plan, unsigned factor)
{
    anvil_ctx_t *ctx = func->owner_ctx;
    unsigned shift = 0;
    while ((1u << shift) < factor)
        shift++;
    anvil_block_t *head = anvil_block_prepare(func, "unroll.header"), *work = anvil_block_prepare(func, "unroll.body");
    if (!head || !work)
        return false;
    anvil_type_t *counting = plan->phis[plan->counter]->result->type;
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, counting, 0), *one = anvil_opt_make_const_int(ctx, counting, 1);
    anvil_value_t *amount = anvil_opt_make_const_int(ctx, counting, shift);
    if (!zero || !one || !amount)
        return false;

    /* The unrolled loop's own counter. */
    anvil_value_t *trips = before_loop(plan, binary(ctx, ANVIL_OP_SUB, counting, zero, plan->starts[plan->counter], "unroll.trips"));
    anvil_value_t *rounds = trips ? before_loop(plan, binary(ctx, ANVIL_OP_SHR, counting, trips, amount, "unroll.rounds")) : NULL;
    anvil_value_t *first = rounds ? before_loop(plan, binary(ctx, ANVIL_OP_SUB, counting, zero, rounds, "unroll.count")) : NULL;
    if (!first)
        return false;
    /* Where each stepped value stands after the unrolled iterations. */
    anvil_value_t *after[RU_PHI_LIMIT];
    for (size_t p = 0; p < plan->num_phis; p++) {
        after[p] = NULL;
        if (!plan->stepped[p])
            continue;
        anvil_type_t *type = plan->phis[p]->result->type;
        anvil_value_t *n = rounds;
        if (!anvil_types_equal(type, counting)) {
            anvil_instr_t *fit = anvil_instr_create(ctx, anvil_type_bit_width(type) < anvil_type_bit_width(counting) ? ANVIL_OP_TRUNC : ANVIL_OP_SEXT,
                                                    type, "unroll.rounds");
            if (!fit || !anvil_instr_add_operand(fit, rounds) || !(n = before_loop(plan, fit)))
                return false;
        }
        anvil_value_t *stride = anvil_opt_make_const_int(ctx, type, plan->steps[p] * (int64_t)factor);
        anvil_value_t *distance = stride ? before_loop(plan, binary(ctx, ANVIL_OP_MUL, type, n, stride, "unroll.distance")) : NULL;
        anvil_instr_t *moved = distance ? binary(ctx, ANVIL_OP_ADD, type, plan->starts[p], distance, "unroll.after") : NULL;
        if (!moved)
            return false;
        const anvil_value_t *original = plan->nexts[p];
        moved->no_wrap = p == plan->counter || (original->kind == ANVIL_VAL_INSTR && original->data.instr->no_wrap);
        if (!(after[p] = before_loop(plan, moved)))
            return false;
    }

    anvil_instr_t *counter = anvil_instr_create(ctx, ANVIL_OP_PHI, counting, "unroll.count");
    anvil_instr_t *phis[RU_PHI_LIMIT];
    anvil_value_t *values[RU_PHI_LIMIT];
    if (!counter)
        return false;
    for (size_t p = 0; p < plan->num_phis; p++) {
        if (!(phis[p] = anvil_instr_create(ctx, ANVIL_OP_PHI, plan->phis[p]->result->type, "unrolled")))
            return false;
        values[p] = phis[p]->result;
    }
    anvil_instr_t *more = binary(ctx, ANVIL_OP_CMP_NE, anvil_type_i1(ctx), counter->result, zero, "unroll.more");
    anvil_instr_t *branch = more ? anvil_instr_create(ctx, ANVIL_OP_BR_COND, ctx->type_void, NULL) : NULL;
    if (!branch || !anvil_instr_add_operand(branch, more->result))
        return false;
    branch->true_block = work;
    branch->false_block = plan->header;

    for (unsigned copy = 0; copy < factor; copy++) {
        anvil_value_t *results[RU_BODY_LIMIT];
        for (size_t i = 0; i < plan->count; i++) {
            const anvil_instr_t *original = plan->instrs[i];
            anvil_instr_t *clone = anvil_instr_create(ctx, original->op, original->result ? original->result->type : ctx->type_void,
                                                      original->result ? original->result->name : NULL);
            if (!clone || !anvil_instr_reserve_operands(clone, original->num_operands) || !anvil_instr_copy_attributes(clone, original))
                return false;
            clone->num_operands = original->num_operands;
            for (size_t o = 0; o < original->num_operands; o++) {
                anvil_value_t *operand = original->operands[o];
                anvil_value_t *mapped = NULL;
                for (size_t p = 0; p < plan->num_phis && !mapped; p++)
                    if (operand == plan->phis[p]->result)
                        mapped = values[p];
                for (size_t c = 0; c < i && !mapped; c++)
                    if (operand == plan->instrs[c]->result)
                        mapped = results[c];
                clone->operands[o] = mapped ? mapped : operand;
            }
            results[i] = clone->result;
            anvil_block_append_prepared(work, clone);
            /* base[v + k*c] as (base + v) + k*c: v stays the index. */
            if (copy > 0 && original->op == ANVIL_OP_GEP && original->num_operands == 2 && original->aux_type && original->result) {
                for (size_t p = 0; p < plan->num_phis; p++) {
                    if (!plan->stepped[p] || original->operands[1] != plan->phis[p]->result)
                        continue;
                    clone->operands[1] = phis[p]->result;
                    anvil_value_t *offset = anvil_opt_make_const_int(ctx, plan->phis[p]->result->type, plan->steps[p] * (int64_t)copy);
                    anvil_instr_t *ahead = offset ? binary(ctx, ANVIL_OP_GEP, original->result->type, clone->result, offset, "unroll.at") : NULL;
                    if (!ahead)
                        return false;
                    ahead->aux_type = original->aux_type;
                    anvil_block_append_prepared(work, ahead);
                    results[i] = ahead->result;
                    break;
                }
            }
        }
        /* The values the next copy reads: a stepped one from the PHI. */
        anvil_value_t *next[RU_PHI_LIMIT];
        for (size_t p = 0; p < plan->num_phis; p++) {
            if (!plan->stepped[p]) {
                const anvil_value_t *latch = plan->nexts[p];
                next[p] = (anvil_value_t *)latch;
                for (size_t q = 0; q < plan->num_phis; q++)
                    if (latch == plan->phis[q]->result)
                        next[p] = values[q];
                for (size_t c = 0; c < plan->count; c++)
                    if (latch == plan->instrs[c]->result)
                        next[p] = results[c];
                continue;
            }
            anvil_type_t *type = plan->phis[p]->result->type;
            anvil_value_t *step = anvil_opt_make_const_int(ctx, type, plan->steps[p] * (int64_t)(copy + 1));
            anvil_instr_t *stepped = step ? binary(ctx, ANVIL_OP_ADD, type, phis[p]->result, step, "unroll.step") : NULL;
            if (!stepped)
                return false;
            const anvil_value_t *original = plan->nexts[p];
            stepped->no_wrap = p == plan->counter || (original->kind == ANVIL_VAL_INSTR && original->data.instr->no_wrap);
            anvil_block_append_prepared(work, stepped);
            next[p] = stepped->result;
        }
        for (size_t p = 0; p < plan->num_phis; p++)
            values[p] = next[p];
    }
    anvil_instr_t *advance = binary(ctx, ANVIL_OP_ADD, counting, counter->result, one, "unroll.count.next");
    anvil_instr_t *back = advance ? anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL) : NULL;
    if (!back)
        return false;
    advance->no_wrap = true;
    back->true_block = head;
    anvil_block_append_prepared(work, advance);
    anvil_block_append_prepared(work, back);

    /* Commit: the new loop's blocks and PHIs, the original loop entered
     * from it. */
    insert_block_after(func, plan->preheader, head);
    insert_block_after(func, head, work);
    anvil_block_append_prepared(head, counter);
    if (!anvil_phi_add_incoming(counter->result, first, plan->preheader) || !anvil_phi_add_incoming(counter->result, advance->result, work))
        return false;
    for (size_t p = 0; p < plan->num_phis; p++) {
        anvil_block_append_prepared(head, phis[p]);
        if (!anvil_phi_add_incoming(phis[p]->result, plan->starts[p], plan->preheader) || !anvil_phi_add_incoming(phis[p]->result, values[p], work))
            return false;
        anvil_instr_t *phi = plan->phis[p];
        for (size_t item = 0; item < phi->num_phi_incoming; item++) {
            if (phi->phi_blocks[item] == plan->preheader) {
                phi->operands[item] = plan->stepped[p] ? after[p] : phis[p]->result;
                phi->phi_blocks[item] = head;
            }
        }
    }
    anvil_block_append_prepared(head, more);
    anvil_block_append_prepared(head, branch);
    plan->preheader->last->true_block = head;
    plan->header->unrolled = true;
    head->unrolled = true;
    anvil_func_invalidate_cfg(func);
    return true;
}

anvil_pass_result_t anvil_pass_runtime_unroll(anvil_func_t *func)
{
    if (!func || !func->owner_ctx || !func->entry)
        return ANVIL_PASS_RUN_ERROR;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(func->owner_ctx);
    unsigned factor = arch ? arch->small_loop_unroll : 1;
    if (factor < 2)
        return ANVIL_PASS_RUN_UNCHANGED;
    if (factor > RU_FACTOR_LIMIT)
        factor = RU_FACTOR_LIMIT;
    while (factor & (factor - 1))
        factor &= factor - 1;  /* a power of two: the rounds are a shift */
    anvil_opt_cfg_t cfg;
    anvil_loop_analysis_t loops;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;
    if (!anvil_loop_analysis_build(&cfg, &loops)) {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }
    /* One loop per run: the pipeline runs again after a change. */
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (size_t loop = 0; loop < loops.count && result == ANVIL_PASS_RUN_UNCHANGED; loop++) {
        ru_plan plan;
        if (!recognize(&cfg, &loops, loop, &plan))
            continue;
        result = unroll(func, &plan, factor) ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_ERROR;
    }
    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    return result;
}
