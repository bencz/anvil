/* Promote nonescaping entry-block scalar allocations to pruned SSA. */
#include "anvil/anvil_opt.h"
#include "anvil/anvil_analysis.h"
#include "opt_utils.h"

#include <stdlib.h>

typedef struct {
    bool defines;
    bool upward_use;
    bool initialized_in;
    bool initialized_out;
    bool live_in;
    anvil_instr_t *phi;
    anvil_value_t *outgoing;
} promotion_block_t;

static bool scalar_type(const anvil_type_t *type)
{
    return type && ((type->kind >= ANVIL_TYPE_I1 && type->kind <= ANVIL_TYPE_F64) || type->kind == ANVIL_TYPE_PTR);
}

static bool collect_accesses(const anvil_opt_cfg_t *cfg, const anvil_value_t *slot, promotion_block_t *states)
{
    for (size_t block = 0; block < cfg->count; block++)
    {
        promotion_block_t *state = &states[block];
        for (anvil_instr_t *instr = cfg->blocks[block]->first; instr; instr = instr->next)
        {
            for (size_t operand = 0; operand < instr->num_operands; operand++)
            {
                if (instr->operands[operand] != slot)
                    continue;
                if (instr->memory_access.is_volatile)
                    return false;

                if (instr->op == ANVIL_OP_LOAD && operand == 0 && anvil_types_equal(instr->result->type, slot->type->data.pointee))
                {
                    if (!state->defines)
                        state->upward_use = true;
                }
                else if (instr->op == ANVIL_OP_STORE && operand == 1 && anvil_types_equal(instr->operands[0]->type, slot->type->data.pointee))
                {
                    state->defines = true;
                }
                else
                {
                    return false;
                }
            }
        }

        state->initialized_out = block != cfg->entry || state->defines;
        state->live_in = state->upward_use;
    }

    return true;
}

/* Meet over all predecessors, including backedges. Starting at true computes
 * definite assignment without inventing a value for uninitialized storage. */
static bool definitely_initialized(const anvil_opt_cfg_t *cfg, promotion_block_t *states)
{
    bool changed;
    do
    {
        changed = false;
        for (size_t rank = 0; rank < cfg->reachable_count; rank++)
        {
            size_t block = cfg->rpo[rank];
            promotion_block_t *state = &states[block];
            bool incoming = block != cfg->entry;
            for (size_t edge = cfg->predecessor_offsets[block]; edge < cfg->predecessor_offsets[block + 1]; edge++)
                incoming = incoming && states[cfg->predecessors[edge]].initialized_out;

            bool outgoing = incoming || state->defines;
            state->initialized_in = incoming;
            if (state->initialized_out != outgoing)
            {
                state->initialized_out = outgoing;
                changed = true;
            }
        }
    } while (changed);

    for (size_t block = 0; block < cfg->count; block++)
    {
        if (states[block].upward_use && !states[block].initialized_in)
            return false;
    }

    return true;
}

static void compute_slot_liveness(const anvil_opt_cfg_t *cfg, promotion_block_t *states)
{
    bool changed;
    do
    {
        changed = false;
        for (size_t rank = cfg->reachable_count; rank > 0; rank--)
        {
            size_t block = cfg->rpo[rank - 1];
            promotion_block_t *state = &states[block];
            if (state->live_in || state->defines)
                continue;

            for (size_t edge = cfg->successor_offsets[block]; edge < cfg->successor_offsets[block + 1]; edge++)
            {
                if (states[cfg->successors[edge]].live_in)
                {
                    state->live_in = true;
                    changed = true;
                    break;
                }
            }
        }
    } while (changed);
}

/* Allocate every PHI before mutating the live IR. Allocation failure leaves
 * the original function intact; detached nodes remain context-owned. */
static bool prepare_phis(anvil_func_t *func, const anvil_opt_cfg_t *cfg, anvil_type_t *type, promotion_block_t *states)
{
    anvil_ctx_t *ctx = func->parent->ctx;
    for (size_t block = 0; block < cfg->count; block++)
    {
        size_t predecessors = cfg->predecessor_offsets[block + 1] - cfg->predecessor_offsets[block];
        if (!states[block].live_in || predecessors < 2)
            continue;

        anvil_instr_t *phi = anvil_instr_create(ctx, ANVIL_OP_PHI, type, "promoted");
        if (!phi)
            return false;

        states[block].phi = phi;
        phi->phi_blocks = anvil_ctx_calloc(ctx, predecessors, sizeof(*phi->phi_blocks));
        if (!phi->phi_blocks || !anvil_instr_reserve_operands(phi, predecessors))
            return false;

        phi->phi_capacity = predecessors;
    }

    return true;
}

static void insert_phis(anvil_func_t *func, const anvil_opt_cfg_t *cfg, promotion_block_t *states)
{
    for (size_t index = 0; index < cfg->count; index++)
    {
        anvil_instr_t *phi = states[index].phi;
        if (!phi)
            continue;

        anvil_block_t *block = cfg->blocks[index];
        phi->parent = block;
        phi->owner_module = func->parent;
        phi->result->owner_module = func->parent;
        phi->next = block->first;
        block->first->prev = phi;
        block->first = phi;
    }
}

static void rename_accesses(anvil_func_t *func, const anvil_opt_cfg_t *cfg, anvil_value_t *slot, promotion_block_t *states)
{
    for (size_t rank = 0; rank < cfg->reachable_count; rank++)
    {
        size_t block = cfg->rpo[rank];
        anvil_instr_t *phi = states[block].phi;
        anvil_value_t *current = phi ? phi->result : NULL;
        if (!phi && block != cfg->entry)
            current = states[cfg->idom[block]].outgoing;

        for (anvil_instr_t *instr = cfg->blocks[block]->first; instr; instr = instr->next)
        {
            if (instr->op == ANVIL_OP_STORE && instr->operands[1] == slot)
            {
                current = instr->operands[0];
                anvil_opt_erase_instr(instr);
            }
            else if (instr->op == ANVIL_OP_LOAD && instr->operands[0] == slot)
            {
                anvil_opt_replace_uses_in_func(func, instr->result, current);
                anvil_opt_erase_instr(instr);
            }
        }

        states[block].outgoing = current;
    }

    for (size_t block = 0; block < cfg->count; block++)
    {
        anvil_instr_t *phi = states[block].phi;
        if (!phi)
            continue;

        for (size_t edge = cfg->predecessor_offsets[block]; edge < cfg->predecessor_offsets[block + 1]; edge++)
        {
            size_t predecessor = cfg->predecessors[edge];
            size_t incoming = phi->num_phi_incoming++;
            phi->phi_blocks[incoming] = cfg->blocks[predecessor];
            phi->operands[incoming] = states[predecessor].outgoing;
        }

        phi->num_operands = phi->num_phi_incoming;
    }
}

static void remove_trivial_phis(anvil_func_t *func, const anvil_opt_cfg_t *cfg, promotion_block_t *states)
{
    bool changed;
    do
    {
        changed = false;
        for (size_t block = 0; block < cfg->count; block++)
        {
            anvil_instr_t *phi = states[block].phi;
            if (!phi || !phi->parent)
                continue;

            anvil_value_t *replacement = NULL;
            bool identical = true;
            for (size_t incoming = 0; incoming < phi->num_operands; incoming++)
            {
                anvil_value_t *value = phi->operands[incoming];
                if (value == phi->result)
                    continue;
                if (replacement && replacement != value)
                {
                    identical = false;
                    break;
                }

                replacement = value;
            }

            if (identical && replacement)
            {
                anvil_opt_replace_uses_in_func(func, phi->result, replacement);
                anvil_opt_erase_instr(phi);
                changed = true;
            }
        }
    } while (changed);
}

anvil_pass_result_t anvil_pass_mem2reg(anvil_func_t *func)
{
    if (!func || !func->parent)
        return ANVIL_PASS_RUN_ERROR;

    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (func->is_declaration || !func->entry)
        return ANVIL_PASS_RUN_UNCHANGED;

    bool candidate = false;
    for (anvil_instr_t *instr = func->entry->first; instr; instr = instr->next)
    {
        if (instr->op == ANVIL_OP_ALLOCA && !instr->num_operands && scalar_type(instr->result->type->data.pointee))
            candidate = true;
    }

    if (!candidate)
        return ANVIL_PASS_RUN_UNCHANGED;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;

    /* CFG cleanup can make these functions eligible on the next iteration.
     * An entry backedge would execute the allocation more than once. */
    if (cfg.reachable_count != cfg.count || cfg.predecessor_offsets[cfg.entry + 1] != cfg.predecessor_offsets[cfg.entry])
    {
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_UNCHANGED;
    }

    anvil_pass_result_t result = ANVIL_PASS_RUN_UNCHANGED;
    for (anvil_instr_t *instr = func->entry->first; instr; instr = instr->next)
    {
        if (instr->op != ANVIL_OP_ALLOCA || instr->num_operands || !scalar_type(instr->result->type->data.pointee))
            continue;

        promotion_block_t *states = anvil_ctx_calloc(ctx, cfg.count, sizeof(*states));
        if (!states)
        {
            result = ANVIL_PASS_RUN_ERROR;
            break;
        }

        if (collect_accesses(&cfg, instr->result, states) && definitely_initialized(&cfg, states))
        {
            compute_slot_liveness(&cfg, states);
            if (!prepare_phis(func, &cfg, instr->result->type->data.pointee, states))
            {
                free(states);
                result = ANVIL_PASS_RUN_ERROR;
                break;
            }

            insert_phis(func, &cfg, states);
            rename_accesses(func, &cfg, instr->result, states);
            remove_trivial_phis(func, &cfg, states);
            anvil_opt_erase_instr(instr);
            result = ANVIL_PASS_RUN_CHANGED;
        }

        free(states);
    }

    anvil_opt_cfg_destroy(&cfg);
    return result;
}
