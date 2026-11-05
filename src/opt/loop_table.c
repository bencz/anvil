/*
 * ANVIL - Tables of loop-computed constants (ANVIL_PASS_LOOP_TABLE)
 *
 * A loop that only computes, from constants, one integer per iteration and
 * stores it to base[k] (k = 0, 1, ... the iteration: a CRC or power table
 * built at run time) is run at compile time: integer operations fold
 * (anvil_opt_fold_integer), selects and branches follow their constant
 * conditions, PHIs take the value of the edge taken.  The values become a
 * read-only table and the store copies from it; the computation, dead,
 * goes, and the copy loop is an ordinary vectorizable loop.  Anything else
 * in the loop (loads, calls, other stores, values it cannot fold) leaves it
 * alone; so do tables beyond LT_TABLE_BYTES and runs beyond LT_STEP_LIMIT.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_analysis.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "loop_utils.h"

#include <stdio.h>
#include <stdlib.h>

enum { LT_TABLE_BYTES = 8192, LT_STEP_LIMIT = 1 << 21, LT_INSTR_LIMIT = 1024 };

typedef struct {
    anvil_instr_t *instr;
    anvil_value_t *value;   /* the constant it holds now (NULL: none yet) */
} lt_slot_t;

typedef struct {
    anvil_ctx_t *ctx;
    lt_slot_t *slots;       /* the loop's instructions, sorted by address */
    size_t count;
} lt_state_t;

static int compare_slots(const void *a, const void *b)
{
    const anvil_instr_t *x = ((const lt_slot_t *)a)->instr, *y = ((const lt_slot_t *)b)->instr;
    return x < y ? -1 : x > y;
}

static lt_slot_t *slot_of(const lt_state_t *state, const anvil_instr_t *instr)
{
    size_t low = 0, high = state->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (state->slots[middle].instr == instr)
            return &state->slots[middle];
        if (state->slots[middle].instr < instr)
            low = middle + 1;
        else
            high = middle;
    }
    return NULL;
}

/* The constant `value` holds in the run (a constant, or a loop value
 * computed already); NULL when it has none. */
static anvil_value_t *constant_of(const lt_state_t *state, anvil_value_t *value)
{
    if (value->kind == ANVIL_VAL_CONST_INT)
        return value;
    if (value->kind != ANVIL_VAL_INSTR || !value->data.instr)
        return NULL;
    lt_slot_t *slot = slot_of(state, value->data.instr);
    return slot ? slot->value : NULL;
}

/* What a run can follow: integer operations anvil_opt_fold_integer
 * folds, selects, PHIs, branches and the store's address. */
static bool simulated(anvil_op_t op)
{
    switch (op) {
    case ANVIL_OP_ADD: case ANVIL_OP_SUB: case ANVIL_OP_MUL: case ANVIL_OP_UMULH: case ANVIL_OP_SMULH:
    case ANVIL_OP_SDIV: case ANVIL_OP_UDIV: case ANVIL_OP_SMOD: case ANVIL_OP_UMOD:
    case ANVIL_OP_AND: case ANVIL_OP_OR: case ANVIL_OP_XOR: case ANVIL_OP_SHL: case ANVIL_OP_SHR: case ANVIL_OP_SAR: case ANVIL_OP_ROTL:
    case ANVIL_OP_CMP_EQ: case ANVIL_OP_CMP_NE: case ANVIL_OP_CMP_LT: case ANVIL_OP_CMP_LE: case ANVIL_OP_CMP_GT: case ANVIL_OP_CMP_GE:
    case ANVIL_OP_CMP_ULT: case ANVIL_OP_CMP_ULE: case ANVIL_OP_CMP_UGT: case ANVIL_OP_CMP_UGE:
    case ANVIL_OP_NEG: case ANVIL_OP_NOT: case ANVIL_OP_TRUNC: case ANVIL_OP_ZEXT: case ANVIL_OP_SEXT:
    case ANVIL_OP_SELECT: case ANVIL_OP_PHI: case ANVIL_OP_BR: case ANVIL_OP_BR_COND: case ANVIL_OP_GEP:
        return true;
    default:
        return false;
    }
}

static bool in_loop_block(const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop, const anvil_block_t *block)
{
    size_t index = block ? anvil_opt_cfg_index(cfg, block) : SIZE_MAX;
    return index != SIZE_MAX && anvil_loop_contains(loops, loop, index);
}

static anvil_pass_result_t table_loop(anvil_func_t *func, const anvil_opt_cfg_t *cfg, const anvil_loop_analysis_t *loops, size_t loop)
{
    const anvil_loop_info_t *info = &loops->loops[loop];
    if (info->latch_count != 1 || info->preheader == SIZE_MAX)
        return ANVIL_PASS_RUN_UNCHANGED;
    anvil_block_t *header = cfg->blocks[info->header], *preheader = cfg->blocks[info->preheader];
    if (!preheader->last || preheader->last->op != ANVIL_OP_BR)
        return ANVIL_PASS_RUN_UNCHANGED;

    /* The loop's instructions: pure integer work, branches, one store. */
    lt_state_t state = {.ctx = func->owner_ctx};
    anvil_instr_t *store = NULL;
    size_t count = 0;
    for (size_t b = 0; b < cfg->count; b++) {
        if (!anvil_loop_contains(loops, loop, b))
            continue;
        for (anvil_instr_t *instr = cfg->blocks[b]->first; instr; instr = instr->next) {
            switch (instr->op) {
            case ANVIL_OP_STORE:
                if (store)
                    return ANVIL_PASS_RUN_UNCHANGED;
                store = instr;
                break;
            default:
                if (!simulated(instr->op))
                    return ANVIL_PASS_RUN_UNCHANGED;
                break;
            }
            count++;
        }
    }
    if (!store || count > LT_INSTR_LIMIT || store->num_operands != 2 || !anvil_type_is_integer(store->operands[0]->type) ||
        store->operands[0]->type->kind == ANVIL_TYPE_I1 || store->memory_access.is_volatile)
        return ANVIL_PASS_RUN_UNCHANGED;
    /* Stored through base[i]: base invariant, the GEP read by the store only. */
    anvil_value_t *address = store->operands[1];
    anvil_instr_t *gep = address->kind == ANVIL_VAL_INSTR ? address->data.instr : NULL;
    anvil_type_t *element = store->operands[0]->type;
    if (!gep || gep->op != ANVIL_OP_GEP || gep->num_operands != 2 || !gep->aux_type || !anvil_types_equal(gep->aux_type, element) ||
        !in_loop_block(cfg, loops, loop, gep->parent) || in_loop_block(cfg, loops, loop, gep->operands[0]->kind == ANVIL_VAL_INSTR ? gep->operands[0]->data.instr->parent : NULL))
        return ANVIL_PASS_RUN_UNCHANGED;

    state.slots = calloc(count ? count : 1, sizeof(*state.slots));
    anvil_value_t **staged = calloc(count ? count : 1, sizeof(*staged));
    size_t capacity = LT_TABLE_BYTES / (element->size ? element->size : 1);
    anvil_value_t **table = calloc(capacity ? capacity : 1, sizeof(*table));
    if (!state.slots || !staged || !table) {
        free(state.slots);
        free(staged);
        free(table);
        return ANVIL_PASS_RUN_ERROR;
    }
    for (size_t b = 0; b < cfg->count; b++)
        if (anvil_loop_contains(loops, loop, b))
            for (anvil_instr_t *instr = cfg->blocks[b]->first; instr; instr = instr->next)
                state.slots[state.count++].instr = instr;
    qsort(state.slots, state.count, sizeof(*state.slots), compare_slots);

    /* Run it: from the preheader's edge into the header until the header's
     * branch leaves the loop. */
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    size_t stored = 0, steps = 0;
    anvil_block_t *from = preheader, *block = header;
    bool done = false, failed = false;
    while (!done && !failed) {
        /* PHIs read the edge taken, all at once. */
        size_t phis = 0;
        for (anvil_instr_t *phi = block->first; phi && phi->op == ANVIL_OP_PHI; phi = phi->next, phis++) {
            staged[phis] = NULL;
            for (size_t item = 0; item < phi->num_phi_incoming; item++)
                if (phi->phi_blocks[item] == from)
                    staged[phis] = constant_of(&state, phi->operands[item]);
            if (!staged[phis])
                failed = true;
        }
        phis = 0;
        anvil_instr_t *instr = block->first;
        for (; instr && instr->op == ANVIL_OP_PHI; instr = instr->next)
            slot_of(&state, instr)->value = staged[phis++];
        anvil_block_t *next = NULL;
        for (; instr && !failed && !next; instr = instr->next) {
            if (++steps > LT_STEP_LIMIT) {
                failed = true;
                break;
            }
            switch (instr->op) {
            case ANVIL_OP_BR:
                next = instr->true_block;
                break;
            case ANVIL_OP_BR_COND: {
                anvil_value_t *condition = constant_of(&state, instr->operands[0]);
                if (!condition)
                    failed = true;
                else
                    next = condition->data.i ? instr->true_block : instr->false_block;
                break;
            }
            case ANVIL_OP_STORE: {
                anvil_value_t *value = constant_of(&state, instr->operands[0]), *index = constant_of(&state, gep->operands[1]);
                if (!value || !index || (uint64_t)index->data.i != stored || stored == capacity)
                    failed = true;
                else
                    table[stored++] = value;
                break;
            }
            case ANVIL_OP_GEP:
                if (instr != gep)
                    failed = true;
                break;
            case ANVIL_OP_SELECT: {
                anvil_value_t *condition = constant_of(&state, instr->operands[0]);
                anvil_value_t *chosen = condition ? constant_of(&state, instr->operands[condition->data.i ? 1 : 2]) : NULL;
                if (!chosen)
                    failed = true;
                else
                    slot_of(&state, instr)->value = chosen;
                break;
            }
            default: {
                anvil_value_t *left = instr->num_operands > 0 ? constant_of(&state, instr->operands[0]) : NULL;
                anvil_value_t *right = instr->num_operands > 1 ? constant_of(&state, instr->operands[1]) : NULL;
                anvil_value_t *folded = left && (instr->num_operands < 2 || right) && instr->num_operands <= 2 && instr->result
                                            ? anvil_opt_fold_integer(state.ctx, instr, left, right)
                                            : NULL;
                if (!folded)
                    failed = true;
                else
                    slot_of(&state, instr)->value = folded;
                break;
            }
            }
        }
        if (failed || !next)
            break;
        if (!in_loop_block(cfg, loops, loop, next))
            done = true;   /* left the loop (only after a whole run) */
        from = block;
        block = next;
    }
    /* Every iteration stored one value; the loop left from its header
     * (values read after it are the header's, known now). */
    if (done && !failed && stored > 0 && from == header) {
        bool outside_ok = true;
        for (anvil_block_t *b = func->blocks; b && outside_ok; b = b->next) {
            if (in_loop_block(cfg, loops, loop, b))
                continue;
            for (anvil_instr_t *user = b->first; user && outside_ok; user = user->next)
                for (size_t o = 0; o < user->num_operands; o++) {
                    anvil_value_t *operand = user->operands[o];
                    if (operand->kind == ANVIL_VAL_INSTR && operand->data.instr && slot_of(&state, operand->data.instr) &&
                        operand->data.instr->parent != header)
                        outside_ok = false;
                }
        }
        if (outside_ok) {
            anvil_ctx_t *ctx = state.ctx;
            anvil_type_t *array = anvil_type_array(ctx, element, stored);
            char name[160];
            snprintf(name, sizeof(name), "%s.table.%u", func->name ? func->name : "loop", (unsigned)header->id);
            anvil_value_t *global = array ? anvil_module_add_global(func->parent, name, array, ANVIL_LINK_INTERNAL) : NULL;
            anvil_value_t *init = global ? anvil_const_array(ctx, element, table, stored) : NULL;
            anvil_instr_t *at = init ? anvil_instr_create(ctx, ANVIL_OP_GEP, address->type, "table.at") : NULL;
            anvil_instr_t *load = at ? anvil_instr_create(ctx, ANVIL_OP_LOAD, element, "table.value") : NULL;
            anvil_value_t *zero = load ? anvil_opt_make_const_int(ctx, gep->operands[1]->type, 0) : NULL;
            if (!zero || !anvil_global_set_initializer(global, init) || !anvil_global_set_constant(global, true) || !anvil_instr_add_operand(at, global) ||
                !anvil_instr_add_operand(at, zero) || !anvil_instr_add_operand(at, gep->operands[1]) || !anvil_instr_add_operand(load, at->result)) {
                result = ANVIL_PASS_RUN_ERROR;
            } else {
                at->aux_type = array;
                anvil_opt_insert_before(store, at);
                anvil_opt_insert_before(store, load);
                store->operands[0] = load->result;
                result = ANVIL_PASS_RUN_CHANGED;
            }
        }
    }
    free(state.slots);
    free(staged);
    free(table);
    return result;
}

anvil_pass_result_t anvil_pass_loop_table(anvil_func_t *func)
{
    if (!func || !func->owner_ctx || !func->entry || !func->parent)
        return ANVIL_PASS_RUN_ERROR;
    anvil_opt_cfg_t cfg;
    anvil_loop_analysis_t loops;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;
    if (!anvil_loop_analysis_build(&cfg, &loops)) {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }
    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (size_t loop = 0; loop < loops.count && result == ANVIL_PASS_RUN_UNCHANGED; loop++)
        result = table_loop(func, &cfg, &loops, loop);
    anvil_loop_analysis_destroy(&loops);
    anvil_opt_cfg_destroy(&cfg);
    if (result == ANVIL_PASS_RUN_CHANGED)
        anvil_func_invalidate_cfg(func);
    return result;
}
