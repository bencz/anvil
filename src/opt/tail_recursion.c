/*
 * ANVIL - Tail-recursion elimination (ANVIL_PASS_TAIL_RECURSION)
 *
 * A function that returns f(args) of itself, or x + f(args) (a wrapping
 * integer add of a value computed before the call), with nothing after the
 * call but that add, needs no new frame for the call: the old entry becomes
 * a loop header whose PHIs take each parameter (the arguments on the back
 * edge) and an accumulator of the x's from zero, every such return becomes
 * a jump back, and every other return adds the accumulator to its value.
 *
 *     fib(n) = n < 2 ? n : fib(n - 1) + fib(n - 2)
 *  becomes  acc = 0; while (n >= 2) { acc += fib(n - 1); n -= 2; } return acc + n
 *
 * A function with allocas (whose frame the recursion would share) or
 * variadic or by-value arguments is left alone.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

enum { TR_SITE_LIMIT = 16 };

typedef struct {
    anvil_block_t *block;       /* holds the call; ends in the return or a jump to it */
    anvil_instr_t *call;
    anvil_instr_t *add;         /* x + call, or NULL */
    anvil_value_t *addend;      /* x, or NULL */
    anvil_instr_t *phi;         /* the return's PHI taking the value from `block`, or NULL */
} tr_site_t;

static bool self_call(const anvil_func_t *func, const anvil_instr_t *instr)
{
    return instr && instr->op == ANVIL_OP_CALL && instr->num_operands == func->num_params + 1 && instr->operands[0] == func->value &&
           !instr->call_arg_attrs;
}

static size_t use_count(const anvil_def_use_t *uses, const anvil_value_t *value)
{
    size_t definition = anvil_def_use_definition(uses, value);
    return definition == SIZE_MAX ? 0 : uses->use_offsets[definition + 1] - uses->use_offsets[definition];
}

/* Whether only `add` (or nothing) and `block`'s terminator follow `call`,
 * and `addend` (when in `block`) comes before it. */
static bool tail_position(const anvil_block_t *block, const anvil_instr_t *call, const anvil_instr_t *add, const anvil_value_t *addend)
{
    if (addend && addend->kind == ANVIL_VAL_INSTR && addend->data.instr->parent == block) {
        bool before = false;
        for (const anvil_instr_t *scan = block->first; scan && scan != call; scan = scan->next)
            before |= scan == addend->data.instr;
        if (!before)
            return false;
    }
    for (const anvil_instr_t *after = call->next; after != block->last; after = after->next) {
        if (after != add)
            return false;
    }
    return true;
}

/* Whether `value`, defined in `block`, is f(args) or x + f(args) with only
 * the add and `block`'s terminator after the call; fills the site. */
static bool match_site(const anvil_func_t *func, const anvil_def_use_t *uses, anvil_block_t *block, anvil_value_t *value, tr_site_t *site)
{
    if (!value || value->kind != ANVIL_VAL_INSTR || value->data.instr->parent != block || use_count(uses, value) != 1)
        return false;
    anvil_instr_t *instr = value->data.instr;
    if (self_call(func, instr) && tail_position(block, instr, NULL, NULL)) {
        *site = (tr_site_t){.block = block, .call = instr};
        return true;
    }
    if (instr->op != ANVIL_OP_ADD || instr->num_operands != 2 || !anvil_type_is_integer(instr->result->type))
        return false;
    /* Either operand may be the tail call (fib(n - 1) + fib(n - 2): the
     * later one); the other is the addend. */
    for (int side = 0; side < 2; side++) {
        anvil_value_t *candidate = instr->operands[side], *addend = instr->operands[1 - side];
        if (candidate->kind != ANVIL_VAL_INSTR || candidate == addend || !self_call(func, candidate->data.instr) ||
            candidate->data.instr->parent != block || use_count(uses, candidate) != 1 ||
            !tail_position(block, candidate->data.instr, instr, addend))
            continue;
        *site = (tr_site_t){.block = block, .call = candidate->data.instr, .add = instr, .addend = addend};
        return true;
    }
    return false;
}

static void remove_incoming(anvil_instr_t *phi, const anvil_block_t *block)
{
    for (size_t item = 0; item < phi->num_phi_incoming; item++) {
        if (phi->phi_blocks[item] != block)
            continue;
        for (size_t next = item + 1; next < phi->num_phi_incoming; next++) {
            phi->operands[next - 1] = phi->operands[next];
            phi->phi_blocks[next - 1] = phi->phi_blocks[next];
        }
        phi->num_phi_incoming--;
        phi->num_operands--;
        return;
    }
}

static size_t find_sites(anvil_func_t *func, const anvil_def_use_t *uses, tr_site_t *sites)
{
    size_t count = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *ret = block->last;
        if (!ret || ret->op != ANVIL_OP_RET)
            continue;
        if (ret->num_operands == 0) {
            /* f(args); ret void */
            if (ret->prev && self_call(func, ret->prev) && count < TR_SITE_LIMIT &&
                (!ret->prev->result || use_count(uses, ret->prev->result) == 0))
                sites[count++] = (tr_site_t){.block = block, .call = ret->prev};
            continue;
        }
        anvil_value_t *value = ret->operands[0];
        if (count < TR_SITE_LIMIT && match_site(func, uses, block, value, &sites[count])) {
            count++;
            continue;
        }
        /* ret phi [W, B]...: each W built in a B that jumps here. */
        anvil_instr_t *phi = value->kind == ANVIL_VAL_INSTR ? value->data.instr : NULL;
        if (!phi || phi->op != ANVIL_OP_PHI || phi->parent != block || use_count(uses, value) != 1)
            continue;
        for (size_t item = 0; item < phi->num_phi_incoming && count < TR_SITE_LIMIT; item++) {
            anvil_block_t *from = phi->phi_blocks[item];
            if (from->last && from->last->op == ANVIL_OP_BR && from->last->true_block == block &&
                match_site(func, uses, from, phi->operands[item], &sites[count])) {
                sites[count].phi = phi;
                count++;
            }
        }
    }
    return count;
}

static bool eligible(const anvil_func_t *func)
{
    if (func->is_declaration || !func->entry || !func->value || !func->type || func->type->data.func.variadic || func->type->data.func.param_attrs)
        return false;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_ALLOCA || instr->op == ANVIL_OP_VA_START)
                return false;
        }
    }
    return true;
}

anvil_pass_result_t anvil_pass_tail_recursion(anvil_func_t *func)
{
    if (!func || !func->owner_ctx)
        return ANVIL_PASS_RUN_ERROR;
    if (!eligible(func))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_ctx_t *ctx = func->owner_ctx;
    anvil_def_use_t uses;
    if (!anvil_def_use_build(func, &uses))
        return ANVIL_PASS_RUN_ERROR;
    tr_site_t sites[TR_SITE_LIMIT];
    size_t count = find_sites(func, &uses, sites);
    anvil_def_use_destroy(&uses);
    if (!count)
        return ANVIL_PASS_RUN_UNCHANGED;
    /* A return PHI fed only by recursive returns would be unreachable. */
    for (size_t site = 0; site < count; site++) {
        size_t taken = 0;
        for (size_t other = 0; other < count; other++)
            taken += sites[site].phi && sites[other].phi == sites[site].phi;
        if (sites[site].phi && taken >= sites[site].phi->num_phi_incoming)
            return ANVIL_PASS_RUN_UNCHANGED;
    }
    bool accumulate = false;
    for (size_t site = 0; site < count; site++)
        accumulate |= sites[site].addend != NULL;
    anvil_type_t *result_type = func->type->data.func.ret;

    /* The entry's code moves to a loop header right after it; the entry
     * keeps only the jump there (names and the entry stay put). */
    anvil_block_t *entry = func->entry;
    anvil_block_t *header = anvil_block_create(func, "tre.header");
    if (!header)
        return ANVIL_PASS_RUN_ERROR;
    anvil_block_t *before = func->blocks;
    while (before && before->next != header)
        before = before->next;
    if (!before)
        return ANVIL_PASS_RUN_ERROR;
    before->next = NULL;
    func->last_block = before == entry ? header : before;
    header->next = entry->next;
    entry->next = header;
    if (func->last_block == entry)
        func->last_block = header;
    header->first = entry->first;
    header->last = entry->last;
    entry->first = entry->last = NULL;
    for (anvil_instr_t *instr = header->first; instr; instr = instr->next)
        instr->parent = header;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr && instr->op == ANVIL_OP_PHI; instr = instr->next) {
            for (size_t item = 0; item < instr->num_phi_incoming; item++) {
                if (instr->phi_blocks[item] == entry)
                    instr->phi_blocks[item] = header;
            }
        }
    }
    for (size_t site = 0; site < count; site++) {
        if (sites[site].block == entry)
            sites[site].block = header;
    }
    anvil_set_insert_point(ctx, entry);
    if (!anvil_build_br(ctx, header))
        return ANVIL_PASS_RUN_ERROR;

    /* Parameters and the accumulator become header PHIs. */
    anvil_instr_t **params = calloc(func->num_params ? func->num_params : 1, sizeof(*params));
    if (!params)
        return ANVIL_PASS_RUN_ERROR;
    anvil_instr_t *anchor = header->first;
    for (size_t index = 0; index < func->num_params; index++) {
        anvil_value_t *param = func->params[index];
        anvil_instr_t *phi = anvil_instr_create(ctx, ANVIL_OP_PHI, param->type, "tre.param");
        if (!phi) {
            free(params);
            return ANVIL_PASS_RUN_ERROR;
        }
        anvil_opt_insert_before(anchor, phi);
        anvil_opt_replace_uses_in_func(func, param, phi->result);
        /* An addend that is the parameter itself now reads its PHI. */
        for (size_t site = 0; site < count; site++) {
            if (sites[site].addend == param)
                sites[site].addend = phi->result;
        }
        if (!anvil_phi_add_incoming(phi->result, param, entry)) {
            free(params);
            return ANVIL_PASS_RUN_ERROR;
        }
        params[index] = phi;
    }
    anvil_instr_t *acc = NULL;
    if (accumulate) {
        anvil_value_t *zero = anvil_opt_make_const_int(ctx, result_type, 0);
        acc = zero ? anvil_instr_create(ctx, ANVIL_OP_PHI, result_type, "tre.acc") : NULL;
        if (!acc) {
            free(params);
            return ANVIL_PASS_RUN_ERROR;
        }
        anvil_opt_insert_before(anchor, acc);
        if (!anvil_phi_add_incoming(acc->result, zero, entry)) {
            free(params);
            return ANVIL_PASS_RUN_ERROR;
        }
    }

    /* Each recursive return jumps back with its arguments. */
    for (size_t index = 0; index < count; index++) {
        tr_site_t *site = &sites[index];
        anvil_instr_t *terminator = site->block->last;
        anvil_value_t *next_acc = acc ? acc->result : NULL;
        if (acc && site->addend) {
            anvil_instr_t *sum = anvil_instr_create(ctx, ANVIL_OP_ADD, result_type, "tre.sum");
            if (!sum || !anvil_instr_add_operand(sum, acc->result) || !anvil_instr_add_operand(sum, site->addend)) {
                free(params);
                return ANVIL_PASS_RUN_ERROR;
            }
            anvil_opt_insert_before(site->call, sum);
            next_acc = sum->result;
        }
        for (size_t param = 0; param < func->num_params; param++) {
            if (!anvil_phi_add_incoming(params[param]->result, site->call->operands[param + 1], site->block)) {
                free(params);
                return ANVIL_PASS_RUN_ERROR;
            }
        }
        if (acc && !anvil_phi_add_incoming(acc->result, next_acc, site->block)) {
            free(params);
            return ANVIL_PASS_RUN_ERROR;
        }
        if (site->phi)
            remove_incoming(site->phi, site->block);
        terminator->op = ANVIL_OP_BR;
        terminator->num_operands = 0;
        terminator->true_block = header;
        terminator->false_block = NULL;
        if (site->add)
            anvil_opt_erase_instr(site->add);
        anvil_opt_erase_instr(site->call);
    }
    free(params);

    /* Every other return adds what the jumps accumulated. */
    for (anvil_block_t *block = func->blocks; acc && block; block = block->next) {
        anvil_instr_t *ret = block->last;
        if (!ret || ret->op != ANVIL_OP_RET || ret->num_operands != 1)
            continue;
        anvil_instr_t *sum = anvil_instr_create(ctx, ANVIL_OP_ADD, result_type, "tre.result");
        if (!sum || !anvil_instr_add_operand(sum, acc->result) || !anvil_instr_add_operand(sum, ret->operands[0]))
            return ANVIL_PASS_RUN_ERROR;
        anvil_opt_insert_before(ret, sum);
        ret->operands[0] = sum->result;
    }
    anvil_func_invalidate_cfg(func);
    return ANVIL_PASS_RUN_CHANGED;
}
