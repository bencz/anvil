/* Speculate only nontrapping integer and address operations into loop
 * preheaders.
 *
 * An address `gep T p, a + b` in a loop, with p and a invariant and b not,
 * splits into `gep T (gep T p, a), b`: the invariant part leaves the loop,
 * and the access left inside indexes an invariant base by b alone (the form
 * loop passes and indexed address modes take).  The split is exact when the
 * index is pointer-wide, or narrower and without wrap: the GEP extends each
 * index in its signedness, and ext(a + b) = ext(a) + ext(b) then. */
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdlib.h>
#include <string.h>

static bool operands_available(const anvil_opt_cfg_t *cfg, const anvil_instr_t *instr, size_t preheader)
{
    for (size_t operand = 0; operand < instr->num_operands; operand++)
    {
        const anvil_value_t *value = instr->operands[operand];
        if (value->kind != ANVIL_VAL_INSTR)
            continue;

        size_t definition = anvil_opt_cfg_index(cfg, value->data.instr->parent);
        if (!anvil_opt_cfg_dominates(cfg, definition, preheader))
            return false;
    }

    return true;
}

static void move_before_terminator(anvil_instr_t *instr, anvil_block_t *preheader)
{
    anvil_opt_erase_instr(instr);
    anvil_instr_t *terminator = preheader->last;
    instr->parent = preheader;
    instr->prev = terminator->prev;
    instr->next = terminator;
    if (terminator->prev)
        terminator->prev->next = instr;
    else
        preheader->first = instr;

    terminator->prev = instr;
}

static bool has_hoistable_root(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop)
{
    size_t header = loops->loops[loop].header;
    if (header == cfg->entry)
        return false;

    for (size_t block = 0; block < cfg->count; block++)
    {
        if (!anvil_loop_contains(loops, loop, block))
            continue;

        for (anvil_instr_t *instr = cfg->blocks[block]->first; instr; instr = instr->next)
        {
            if (!instr->result || !anvil_opt_speculatable(instr))
                continue;

            bool available = true;
            for (size_t operand = 0; operand < instr->num_operands; operand++)
            {
                const anvil_value_t *value = instr->operands[operand];
                if (value->kind != ANVIL_VAL_INSTR)
                    continue;

                size_t definition = anvil_opt_cfg_index(cfg, value->data.instr->parent);
                if (anvil_loop_contains(loops, loop, definition) || !anvil_opt_cfg_dominates(cfg, definition, header))
                {
                    available = false;
                    break;
                }
            }

            if (available)
                return true;
        }
    }

    return false;
}

static bool defined_outside(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, size_t preheader,
                            const anvil_value_t *value)
{
    if (value->kind != ANVIL_VAL_INSTR)
        return true;
    size_t definition = anvil_opt_cfg_index(cfg, value->data.instr->parent);
    return !anvil_loop_contains(loops, loop, definition) && anvil_opt_cfg_dominates(cfg, definition, preheader);
}

/* Splits `gep` (see the header) with its invariant part placed in the
 * preheader; false when it has no such form. */
static bool split_invariant_index(anvil_ctx_t *ctx, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop,
                                  size_t preheader, size_t pointer_bits, anvil_instr_t *gep)
{
    if (gep->op != ANVIL_OP_GEP || gep->num_operands != 2 || !gep->result ||
        !defined_outside(cfg, loops, loop, preheader, gep->operands[0]))
        return false;
    anvil_value_t *index = gep->operands[1];
    anvil_instr_t *sum = index->kind == ANVIL_VAL_INSTR ? index->data.instr : NULL;
    if (!sum || sum->op != ANVIL_OP_ADD || sum->num_operands != 2 || !anvil_memory_index_distributes(sum, pointer_bits / 8))
        return false;
    size_t side = defined_outside(cfg, loops, loop, preheader, sum->operands[0]) ? 0
                : defined_outside(cfg, loops, loop, preheader, sum->operands[1]) ? 1 : 2;
    if (side == 2 || defined_outside(cfg, loops, loop, preheader, sum->operands[1 - side]))
        return false;
    return anvil_opt_split_gep(ctx, gep, cfg->blocks[preheader]->last, sum->operands[side], sum->operands[1 - side]);
}

enum { PROMOTE_ACCESS_LIMIT = 64, PROMOTE_EXIT_LIMIT = 4 };


static anvil_type_t *access_type(const anvil_instr_t *access)
{
    return access->op == ANVIL_OP_LOAD ? access->result->type : access->operands[0]->type;
}

static size_t access_size(const anvil_instr_t *access)
{
    return access_type(access)->size;
}

static anvil_value_t *access_address(const anvil_instr_t *access)
{
    return access->op == ANVIL_OP_LOAD ? access->operands[0] : access->operands[1];
}


/* Scalar promotion: a cell the loop reaches through one invariant address
 * and nothing else in the loop may touch lives in a register.  Its value
 * goes to a stack slot, loaded before the loop and, if the loop stores it,
 * written back on each exit where a store happened (a flag slot: a store
 * the program did not make would be a data race); mem2reg then makes the
 * slots values.  The loop calls nothing that touches memory and has no
 * atomic or volatile access; the load before the loop cannot fault, as the
 * header (run on every entry) accesses the cell or its range is a known
 * object's. */
typedef struct {
    anvil_instr_t *accesses[PROMOTE_ACCESS_LIMIT];
    bool group[PROMOTE_ACCESS_LIMIT];
    size_t count;
    anvil_value_t *address;
    anvil_type_t *type;
    bool stored;
    anvil_instr_t *template_load, *template_store;
    size_t from[PROMOTE_EXIT_LIMIT], to[PROMOTE_EXIT_LIMIT], exits;
} promotion_plan;

/* The first cell of `loop` that may be promoted; nothing is changed.  An
 * address from outside the loop that dominates its header dominates the
 * preheader too, so a loop without one yet can be planned. */
static bool plan_promotion(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, promotion_plan *plan)
{
    plan->count = 0;
    size_t header = loops->loops[loop].header;
    /* mem2reg keeps the slots in values only when the entry has no
     * predecessors; a loop headed by the entry has no preheader. */
    if (header == cfg->entry || cfg->predecessor_offsets[cfg->entry + 1] != cfg->predecessor_offsets[cfg->entry])
        return false;
    for (size_t block = 0; block < cfg->count; block++)
    {
        if (!anvil_loop_contains(loops, loop, block))
            continue;
        for (anvil_instr_t *instr = cfg->blocks[block]->first; instr; instr = instr->next)
        {
            if (anvil_op_is_atomic(instr->op) || instr->memory_access.is_volatile || instr->op == ANVIL_OP_VA_START ||
                (instr->op == ANVIL_OP_CALL && (anvil_opt_call_observes_memory(instr) || (anvil_opt_call_effects(instr) & ANVIL_EFFECT_WRITE_MEMORY))))
                return false;
            if (instr->op != ANVIL_OP_LOAD && (instr->op != ANVIL_OP_STORE || instr->num_operands != 2))
                continue;
            if (plan->count == PROMOTE_ACCESS_LIMIT)
                return false;
            plan->accesses[plan->count++] = instr;
        }
    }

    for (size_t candidate = 0; candidate < plan->count; candidate++)
    {
        anvil_instr_t *first = plan->accesses[candidate];
        anvil_value_t *address = access_address(first);
        anvil_type_t *type = access_type(first);
        size_t size = access_size(first);
        size_t definition = address->kind == ANVIL_VAL_INSTR ? anvil_opt_cfg_index(cfg, address->data.instr->parent) : SIZE_MAX;
        if (!size || !anvil_opt_scalar_type(type) ||
            (definition != SIZE_MAX && (anvil_loop_contains(loops, loop, definition) || !anvil_opt_cfg_dominates(cfg, definition, header))) ||
            (address->kind == ANVIL_VAL_INSTR && address->data.instr->op == ANVIL_OP_ALLOCA))
            continue;
        bool valid = true, safe = false;
        plan->address = address;
        plan->type = type;
        plan->stored = false;
        plan->template_load = plan->template_store = NULL;
        for (size_t other = 0; other < plan->count && valid; other++)
        {
            anvil_instr_t *access = plan->accesses[other];
            anvil_alias_result_t alias = anvil_memory_alias(access_address(access), access_size(access), address, size);
            plan->group[other] = alias == ANVIL_ALIAS_MUST && anvil_types_equal(access_type(access), type);
            if (plan->group[other])
            {
                plan->stored |= access->op == ANVIL_OP_STORE;
                safe |= anvil_opt_cfg_index(cfg, access->parent) == header;
                if (access->op == ANVIL_OP_LOAD && !plan->template_load)
                    plan->template_load = access;
                if (access->op == ANVIL_OP_STORE && !plan->template_store)
                    plan->template_store = access;
            }
            else if (alias != ANVIL_ALIAS_NO)
            {
                valid = false;
            }
        }
        const anvil_value_t *object;
        size_t offset;
        if (!valid || (!safe && !anvil_memory_bounded_range(address, size, &object, &offset)))
            continue;

        /* The loop's exit edges, where a stored cell is written back. */
        plan->exits = 0;
        bool fits = true;
        for (size_t block = 0; block < cfg->count && fits && plan->stored; block++)
        {
            if (!anvil_loop_contains(loops, loop, block))
                continue;
            for (size_t edge = cfg->successor_offsets[block]; edge < cfg->successor_offsets[block + 1]; edge++)
            {
                size_t successor = cfg->successors[edge];
                bool seen = false;
                for (size_t index = 0; index < plan->exits; index++)
                    seen |= plan->from[index] == block && plan->to[index] == successor;
                if (anvil_loop_contains(loops, loop, successor) || seen)
                    continue;
                if (plan->exits == PROMOTE_EXIT_LIMIT)
                {
                    fits = false;
                    break;
                }
                plan->from[plan->exits] = block;
                plan->to[plan->exits++] = successor;
            }
        }
        if (fits)
            return true;
    }
    return false;
}

/* Applies the plan: everything is created before the IR changes. */
static anvil_pass_result_t promote_memory(anvil_ctx_t *ctx, anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops,
                                          size_t loop, size_t preheader)
{
    promotion_plan plan;
    if (!plan_promotion(cfg, loops, loop, &plan))
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_value_t *address = plan.address;
    anvil_type_t *type = plan.type;
    anvil_type_t *pointer = anvil_type_ptr(ctx, type), *flag_type = anvil_type_i1(ctx), *flag_pointer = anvil_type_ptr(ctx, flag_type);
    anvil_value_t *yes = anvil_const_i1(ctx, true), *no = anvil_const_i1(ctx, false);
    anvil_instr_t *slot = pointer ? anvil_instr_create(ctx, ANVIL_OP_ALLOCA, pointer, "promoted.slot") : NULL;
    anvil_instr_t *flag = plan.stored && flag_pointer ? anvil_instr_create(ctx, ANVIL_OP_ALLOCA, flag_pointer, "promoted.stored") : NULL;
    anvil_instr_t *initial = anvil_opt_make_instr(ctx, ANVIL_OP_LOAD, type, address, NULL, "promoted");
    anvil_instr_t *keep = slot && initial ? anvil_opt_make_instr(ctx, ANVIL_OP_STORE, ctx->type_void, initial->result, slot->result, NULL) : NULL;
    anvil_instr_t *clear = flag && no ? anvil_opt_make_instr(ctx, ANVIL_OP_STORE, ctx->type_void, no, flag->result, NULL) : NULL;
    if (!keep || (plan.stored && (!clear || !yes)) || (plan.template_load && !anvil_instr_copy_attributes(initial, plan.template_load)))
        return ANVIL_PASS_RUN_ERROR;
    anvil_instr_t *marks[PROMOTE_ACCESS_LIMIT] = { NULL };
    for (size_t index = 0; index < plan.count && plan.stored; index++)
    {
        if (plan.group[index] && plan.accesses[index]->op == ANVIL_OP_STORE && !(marks[index] = anvil_opt_make_instr(ctx, ANVIL_OP_STORE, ctx->type_void, yes, flag->result, NULL)))
            return ANVIL_PASS_RUN_ERROR;
    }
    /* Each exit edge: test the flag, write back where a store happened. */
    anvil_block_t *tests[PROMOTE_EXIT_LIMIT], *writes[PROMOTE_EXIT_LIMIT], *joins[PROMOTE_EXIT_LIMIT];
    for (size_t exit = 0; exit < plan.exits; exit++)
    {
        tests[exit] = anvil_block_prepare(func, "promoted.exit");
        writes[exit] = anvil_block_prepare(func, "promoted.write");
        joins[exit] = anvil_block_prepare(func, "promoted.join");
        anvil_instr_t *test = flag ? anvil_opt_make_instr(ctx, ANVIL_OP_LOAD, flag_type, flag->result, NULL, "promoted") : NULL;
        anvil_instr_t *branch = test ? anvil_opt_make_instr(ctx, ANVIL_OP_BR_COND, ctx->type_void, test->result, NULL, NULL) : NULL;
        anvil_instr_t *value = anvil_opt_make_instr(ctx, ANVIL_OP_LOAD, type, slot->result, NULL, "promoted");
        anvil_instr_t *write = value ? anvil_opt_make_instr(ctx, ANVIL_OP_STORE, ctx->type_void, value->result, address, NULL) : NULL;
        anvil_instr_t *to_join = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
        anvil_instr_t *to_exit = anvil_instr_create(ctx, ANVIL_OP_BR, ctx->type_void, NULL);
        if (!tests[exit] || !writes[exit] || !joins[exit] || !branch || !write || !to_join || !to_exit ||
            !anvil_instr_copy_attributes(write, plan.template_store))
            return ANVIL_PASS_RUN_ERROR;
        branch->true_block = writes[exit];
        branch->false_block = joins[exit];
        to_join->true_block = joins[exit];
        to_exit->true_block = cfg->blocks[plan.to[exit]];
        anvil_block_append_prepared(tests[exit], test);
        anvil_block_append_prepared(tests[exit], branch);
        anvil_block_append_prepared(writes[exit], value);
        anvil_block_append_prepared(writes[exit], write);
        anvil_block_append_prepared(writes[exit], to_join);
        anvil_block_append_prepared(joins[exit], to_exit);
    }

    anvil_instr_t *entry_first = func->entry->first;
    anvil_opt_insert_before(entry_first, slot);
    if (flag)
        anvil_opt_insert_before(entry_first, flag);
    anvil_block_t *before = cfg->blocks[preheader];
    anvil_opt_insert_before(before->last, initial);
    anvil_opt_insert_before(before->last, keep);
    if (clear)
        anvil_opt_insert_before(before->last, clear);
    for (size_t index = 0; index < plan.count; index++)
    {
        if (!plan.group[index])
            continue;
        anvil_instr_t *access = plan.accesses[index];
        if (access->op == ANVIL_OP_LOAD)
        {
            access->operands[0] = slot->result;
            continue;
        }
        access->operands[1] = slot->result;
        if (access->next)
            anvil_opt_insert_before(access->next, marks[index]);
    }
    for (size_t exit = 0; exit < plan.exits; exit++)
    {
        anvil_block_t *source = cfg->blocks[plan.from[exit]], *target = cfg->blocks[plan.to[exit]];
        anvil_func_append_block(func, tests[exit]);
        anvil_func_append_block(func, writes[exit]);
        anvil_func_append_block(func, joins[exit]);
        anvil_opt_redirect_edge(source->last, target, tests[exit]);
        for (anvil_instr_t *phi = target->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next)
        {
            for (size_t incoming = 0; incoming < phi->num_phi_incoming; incoming++)
            {
                if (phi->phi_blocks[incoming] == source)
                    phi->phi_blocks[incoming] = joins[exit];
            }
        }
    }
    anvil_func_invalidate_cfg(func);
    return ANVIL_PASS_RUN_CHANGED;
}

anvil_pass_result_t anvil_pass_licm(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->blocks)
        return ANVIL_PASS_RUN_UNCHANGED;

    bool changed = false;
    const anvil_arch_info_t *arch = anvil_ctx_get_arch_info(ctx);
    size_t pointer_bits = arch ? arch->ptr_size * 8 : 64;
    anvil_opt_cfg_t cfg;
    anvil_loop_analysis_t loops;

rebuild:
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;

    if (!anvil_loop_analysis_build(&cfg, &loops))
    {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }

    for (size_t loop = 0; loop < loops.count; loop++)
    {
        size_t preheader = loops.loops[loop].preheader;
        if (preheader == SIZE_MAX)
        {
            /* Avoid creating empty preheaders that simplify-cfg would remove
             * again on every fixed-point iteration. */
            promotion_plan plan;
            if (!has_hoistable_root(&cfg, &loops, loop) && !plan_promotion(&cfg, &loops, loop, &plan))
                continue;

            anvil_block_t *created = anvil_opt_create_preheader(&cfg, &loops, loop);
            anvil_loop_analysis_destroy(&loops);
            anvil_opt_cfg_destroy(&cfg);
            if (!created)
                return ANVIL_PASS_RUN_ERROR;

            changed = true;
            goto rebuild;
        }

        bool moved;
        do
        {
            moved = false;
            for (size_t index = 0; index < cfg.reachable_count; index++)
            {
                size_t block = cfg.rpo[index];
                if (!anvil_loop_contains(&loops, loop, block))
                    continue;

                anvil_instr_t *instr = cfg.blocks[block]->first;
                while (instr)
                {
                    anvil_instr_t *next = instr->next;
                    if (instr->result && anvil_opt_speculatable(instr) && operands_available(&cfg, instr, preheader))
                    {
                        move_before_terminator(instr, cfg.blocks[preheader]);
                        moved = true;
                        changed = true;
                    }
                    else if (split_invariant_index(ctx, &cfg, &loops, loop, preheader, pointer_bits, instr))
                    {
                        changed = true;
                    }
                    else if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
                    {
                        anvil_loop_analysis_destroy(&loops);
                        anvil_opt_cfg_destroy(&cfg);
                        return ANVIL_PASS_RUN_ERROR;
                    }

                    instr = next;
                }
            }
        } while (moved);

        anvil_pass_result_t promoted = promote_memory(ctx, func, &cfg, &loops, loop, preheader);
        if (promoted != ANVIL_PASS_RUN_UNCHANGED)
        {
            anvil_loop_analysis_destroy(&loops);
            anvil_opt_cfg_destroy(&cfg);
            /* The slots become values now, so an enclosing loop sees the
             * cell's accesses before and after this loop as its own. */
            if (promoted == ANVIL_PASS_RUN_ERROR || anvil_pass_mem2reg(func) == ANVIL_PASS_RUN_ERROR || anvil_pass_copy_prop(func) == ANVIL_PASS_RUN_ERROR)
                return ANVIL_PASS_RUN_ERROR;
            changed = true;
            goto rebuild;
        }
    }

    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
