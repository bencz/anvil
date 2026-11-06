/* Counting loops in closed form.
 *
 * A loop of a header and one body block, the header testing a counter
 * against zero and the body stepping it by one toward zero (modulo its
 * width, so it always gets there after T steps: T = c for c - 1, T = -c for
 * c + 1), whose body only adds invariants to its other PHIs (v + k) or
 * sets them to invariants, and adds invariants to cells (*p + k, a load,
 * the add and a store), runs T times what one closed
 * form does once: v + k * T, *p + k * T.  The body becomes that form with
 * the counter set to zero, so the header's test leaves on the next check;
 * the loop is entered, and the cells written, exactly when it was before.
 * The cells' updates keep their order, so equal addresses accumulate alike;
 * addresses that may partly overlap are rejected.  The arithmetic wraps:
 * no-wrap steps stay exact, and the closed form has the same final value. */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"

#include <stdlib.h>

enum { CLOSED_FORM_LIMIT = 16 };

typedef struct {
    anvil_instr_t *update;      /* v +- k, or the cell's add */
    anvil_instr_t *phi;         /* the PHI, or NULL for a cell */
    anvil_instr_t *load;        /* a cell's load (its store follows the add) */
    anvil_instr_t *store;
    size_t addend;              /* the update's invariant operand */
} closed_term_t;

typedef struct {
    anvil_block_t *header, *body;
    anvil_instr_t *counter, *step;
    bool down;                  /* c - 1: T = c; c + 1: T = -c */
    closed_term_t terms[CLOSED_FORM_LIMIT];
    size_t term_count;
} closed_loop_t;

static bool invariant(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_value_t *value)
{
    return value->kind != ANVIL_VAL_INSTR || !anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, value->data.instr->parent));
}

/* An add or sub of an invariant to `value`: the invariant operand's index. */
static bool adds_invariant(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_instr_t *instr,
                           const anvil_value_t *value, size_t *addend)
{
    if (instr->num_operands != 2 || !anvil_type_is_integer(instr->result->type) || instr->result->type->kind == ANVIL_TYPE_I1)
        return false;
    if (instr->op == ANVIL_OP_ADD && instr->operands[1] == value && invariant(cfg, loops, loop, instr->operands[0]))
        *addend = 0;
    else if ((instr->op == ANVIL_OP_ADD || instr->op == ANVIL_OP_SUB) && instr->operands[0] == value && invariant(cfg, loops, loop, instr->operands[1]))
        *addend = 1;
    else
        return false;
    return true;
}

/* Whether, inside the loop, only `first` and `second` read `value` (what
 * the loop leaves to the code after it is the PHI's final value). */
static bool read_inside_only_by(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, const anvil_def_use_t *uses, size_t loop,
                                const anvil_value_t *value, const anvil_instr_t *first, const anvil_instr_t *second)
{
    size_t definition = anvil_def_use_definition(uses, value);
    if (definition == SIZE_MAX)
        return false;
    for (size_t use = uses->use_offsets[definition]; use < uses->use_offsets[definition + 1]; use++)
    {
        const anvil_instr_t *user = uses->instructions[uses->users[use]];
        if (user != first && user != second && anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, user->parent)))
            return false;
    }
    return true;
}

static bool recognize(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, const anvil_def_use_t *uses, size_t loop, closed_loop_t *plan)
{
    if (loops->loops[loop].member_count != 2 || loops->loops[loop].latch_count != 1)
        return false;
    anvil_block_t *header = cfg->blocks[loops->loops[loop].header];
    anvil_instr_t *branch = header->last;
    if (!branch || branch->op != ANVIL_OP_BR_COND || branch->operands[0]->kind != ANVIL_VAL_INSTR)
        return false;
    anvil_instr_t *test = branch->operands[0]->data.instr;
    bool inside_true = anvil_loop_contains(loops, loop, anvil_opt_cfg_index(cfg, branch->true_block));
    plan->header = header;
    plan->body = inside_true ? branch->true_block : branch->false_block;
    if (test->parent != header || test->next != branch || test->num_operands != 2 || plan->body == header ||
        (test->op != (inside_true ? ANVIL_OP_CMP_NE : ANVIL_OP_CMP_EQ)) || anvil_def_use_count(uses, test->result) != 1 ||
        !plan->body->last || plan->body->last->op != ANVIL_OP_BR || plan->body->last->true_block != header)
        return false;
    size_t zero_side = anvil_opt_is_zero(test->operands[1]) ? 1 : anvil_opt_is_zero(test->operands[0]) ? 0 : 2;
    if (zero_side == 2 || test->operands[1 - zero_side]->kind != ANVIL_VAL_INSTR)
        return false;
    plan->counter = test->operands[1 - zero_side]->data.instr;
    if (plan->counter->op != ANVIL_OP_PHI || plan->counter->parent != header)
        return false;

    /* The header holds its PHIs, the test and the branch; every PHI comes
     * from outside and from the body. */
    plan->term_count = 0;
    plan->step = NULL;
    for (anvil_instr_t *instr = header->first; instr != test; instr = instr->next)
    {
        if (instr->op != ANVIL_OP_PHI || instr->num_phi_incoming != 2)
            return false;
        size_t back = instr->phi_blocks[0] == plan->body ? 0 : instr->phi_blocks[1] == plan->body ? 1 : 2;
        if (back == 2)
            return false;
        /* A value the body sets to an invariant holds it after any number
         * of passes, the closed form's one included. */
        if (instr != plan->counter && invariant(cfg, loops, loop, instr->operands[back]))
            continue;
        if (instr->operands[back]->kind != ANVIL_VAL_INSTR || instr->operands[back]->data.instr->parent != plan->body)
            return false;
        anvil_instr_t *update = instr->operands[back]->data.instr;
        size_t addend;
        if (!adds_invariant(cfg, loops, loop, update, instr->result, &addend) || anvil_def_use_count(uses, update->result) != 1)
            return false;
        if (instr == plan->counter)
        {
            const anvil_value_t *step = update->operands[addend];
            if (step->kind != ANVIL_VAL_CONST_INT || (!anvil_opt_is_one(step) && !anvil_opt_is_all_ones(step)) || update->operands[1 - addend] != instr->result)
                return false;
            plan->step = update;
            plan->down = anvil_opt_is_one(step) == (update->op == ANVIL_OP_SUB);
            continue;
        }
        if (plan->term_count == CLOSED_FORM_LIMIT || !read_inside_only_by(cfg, loops, uses, loop, instr->result, update, update))
            return false;
        plan->terms[plan->term_count++] = (closed_term_t){ .update = update, .phi = instr, .addend = addend };
    }
    if (!plan->step || !read_inside_only_by(cfg, loops, uses, loop, plan->counter->result, test, plan->step))
        return false;

    /* The body: the PHIs' updates and cells' load-add-store, nothing else. */
    for (anvil_instr_t *instr = plan->body->first; instr != plan->body->last; instr = instr->next)
    {
        bool known = instr == plan->step;
        for (size_t index = 0; index < plan->term_count && !known; index++)
            known = instr == plan->terms[index].update;
        if (known)
            continue;
        anvil_instr_t *add = instr->op == ANVIL_OP_LOAD && !instr->memory_access.is_volatile && anvil_def_use_count(uses, instr->result) == 1 &&
                             instr->next && instr->next->op != ANVIL_OP_BR ? instr->next : NULL;
        anvil_instr_t *store = add ? add->next : NULL;
        size_t addend;
        if (!add || !store || store->op != ANVIL_OP_STORE || store->num_operands != 2 || store->memory_access.is_volatile ||
            store->operands[0] != add->result || store->operands[1] != instr->operands[0] || !invariant(cfg, loops, loop, instr->operands[0]) ||
            !adds_invariant(cfg, loops, loop, add, instr->result, &addend) || anvil_def_use_count(uses, add->result) != 1 ||
            plan->term_count == CLOSED_FORM_LIMIT)
            return false;
        plan->terms[plan->term_count++] = (closed_term_t){ .update = add, .load = instr, .store = store, .addend = addend };
        instr = store;
    }

    /* Cells at equal addresses accumulate in order; a partial overlap does not. */
    for (size_t first = 0; first < plan->term_count; first++)
        for (size_t second = first + 1; second < plan->term_count; second++)
        {
            const closed_term_t *a = &plan->terms[first], *b = &plan->terms[second];
            if (!a->load || !b->load)
                continue;
            anvil_alias_result_t alias = anvil_memory_alias(a->load->operands[0], a->load->result->type->size, b->load->operands[0], b->load->result->type->size);
            if (alias == ANVIL_ALIAS_MAY || (alias == ANVIL_ALIAS_MUST && !anvil_types_equal(a->load->result->type, b->load->result->type)))
                return false;
        }
    return true;
}


/* The body runs once as the closed form; the counter leaves at zero.  All
 * instructions are made before the body changes. */
static bool rewrite(anvil_ctx_t *ctx, closed_loop_t *plan)
{
    anvil_type_t *count_type = plan->counter->result->type;
    anvil_value_t *zero = anvil_opt_make_const_int(ctx, count_type, 0);
    anvil_instr_t *made[4 * CLOSED_FORM_LIMIT + 2];
    size_t made_count = 0;
    anvil_value_t *trips = plan->counter->result;
    if (!zero)
        return false;
    if (!plan->down)
    {
        anvil_instr_t *negate = anvil_opt_make_instr(ctx, ANVIL_OP_SUB, count_type, zero, trips, "closed");
        if (!negate)
            return false;
        made[made_count++] = negate;
        trips = negate->result;
    }
    anvil_value_t *values[CLOSED_FORM_LIMIT];
    size_t first_made[CLOSED_FORM_LIMIT + 1];
    for (size_t index = 0; index < plan->term_count; index++)
    {
        closed_term_t *term = &plan->terms[index];
        first_made[index] = made_count;
        anvil_type_t *type = term->update->result->type;
        anvil_value_t *count = trips;
        if (type->size != count_type->size)
        {
            anvil_instr_t *cast = anvil_opt_make_instr(ctx, type->size > count_type->size ? ANVIL_OP_ZEXT : ANVIL_OP_TRUNC, type, trips, NULL, "closed");
            if (!cast)
                return false;
            made[made_count++] = cast;
            count = cast->result;
        }
        else if (!anvil_types_equal(type, count_type))
        {
            anvil_instr_t *cast = anvil_opt_make_instr(ctx, ANVIL_OP_BITCAST, type, trips, NULL, "closed");
            if (!cast)
                return false;
            made[made_count++] = cast;
            count = cast->result;
        }
        anvil_value_t *addend = term->update->operands[term->addend];
        anvil_instr_t *product = anvil_opt_is_one(addend) ? NULL : anvil_opt_make_instr(ctx, ANVIL_OP_MUL, type, addend, count, "closed");
        if (!anvil_opt_is_one(addend) && !product)
            return false;
        if (product)
            made[made_count++] = product;
        anvil_value_t *base = term->update->operands[1 - term->addend];
        anvil_instr_t *load = NULL;
        if (term->load)
        {
            load = anvil_opt_make_instr(ctx, ANVIL_OP_LOAD, type, term->load->operands[0], NULL, "closed");
            if (!load || !anvil_instr_copy_attributes(load, term->load))
                return false;
            made[made_count++] = load;
            base = load->result;
        }
        anvil_instr_t *total = anvil_opt_make_instr(ctx, term->update->op, type, base, product ? product->result : count, "closed");
        if (!total)
            return false;
        made[made_count++] = total;
        values[index] = total->result;
    }
    first_made[plan->term_count] = made_count;

    /* Commit: the new form goes before the body's branch, term by term in
     * the body's order (a cell's store before the next term's load), and
     * the old work goes. */
    anvil_instr_t *branch = plan->body->last;
    size_t first_term = plan->term_count ? first_made[0] : made_count;
    for (size_t index = 0; index < first_term; index++)
        anvil_opt_insert_before(branch, made[index]);
    for (size_t index = 0; index < plan->term_count; index++)
    {
        closed_term_t *term = &plan->terms[index];
        for (size_t item = first_made[index]; item < first_made[index + 1]; item++)
            anvil_opt_insert_before(branch, made[item]);
        if (term->store)
        {
            term->store->operands[0] = values[index];
            anvil_opt_erase_instr(term->store);
            anvil_opt_insert_before(branch, term->store);
            anvil_opt_erase_instr(term->update);
            anvil_opt_erase_instr(term->load);
            continue;
        }
        for (size_t incoming = 0; incoming < term->phi->num_phi_incoming; incoming++)
            if (term->phi->phi_blocks[incoming] == plan->body)
                term->phi->operands[incoming] = values[index];
        anvil_opt_erase_instr(term->update);
    }
    for (size_t incoming = 0; incoming < plan->counter->num_phi_incoming; incoming++)
        if (plan->counter->phi_blocks[incoming] == plan->body)
            plan->counter->operands[incoming] = zero;
    anvil_opt_erase_instr(plan->step);
    return true;
}

anvil_pass_result_t anvil_pass_closed_form(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (;;)
    {
        anvil_opt_cfg_t cfg;
        anvil_loop_analysis_t loops;
        anvil_def_use_t uses;
        if (!anvil_opt_cfg_build(func, &cfg))
            return ANVIL_PASS_RUN_ERROR;
        if (!anvil_loop_analysis_build(&cfg, &loops))
        {
            anvil_opt_cfg_destroy(&cfg);
            return ANVIL_PASS_RUN_ERROR;
        }
        if (!anvil_def_use_build(func, &uses))
        {
            anvil_loop_analysis_destroy(&loops);
            anvil_opt_cfg_destroy(&cfg);
            return ANVIL_PASS_RUN_ERROR;
        }
        bool rewritten = false, failed = false;
        for (size_t loop = 0; loop < loops.count && !rewritten; loop++)
        {
            closed_loop_t plan;
            if (!recognize(&cfg, &loops, &uses, loop, &plan))
                continue;
            failed = !rewrite(ctx, &plan);
            rewritten = !failed;
            break;
        }
        anvil_def_use_destroy(&uses);
        anvil_loop_analysis_destroy(&loops);
        anvil_opt_cfg_destroy(&cfg);
        if (failed)
            return ANVIL_PASS_RUN_ERROR;
        if (!rewritten)
            return result;
        result = ANVIL_PASS_RUN_CHANGED;
    }
}
