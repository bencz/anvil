/*
 * ANVIL - Redundant Load Elimination Pass
 * 
 * Eliminates redundant loads from the same memory location.
 * If a value has already been loaded and the memory hasn't been
 * modified, reuse the loaded value instead of loading again.
 * 
 * Example:
 *   x = *p
 *   y = *p
 * Becomes:
 *   x = *p
 *   y = x
 * 
 * The second load is eliminated and replaced with a copy.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include "anvil/anvil_analysis.h"
#include <stdlib.h>
#include <string.h>

/* Check if instruction may modify memory at pointer p */
static bool may_modify_ptr(anvil_instr_t *instr, anvil_value_t *ptr)
{
    if (!instr) return false;
    if (anvil_op_is_atomic(instr->op))
        return true;

    
    /* Store to same pointer */
    if (instr->op == ANVIL_OP_STORE) {
        if (instr->num_operands > 1) {
            anvil_value_t *store_ptr = instr->operands[1];
            anvil_type_t *object = anvil_sem_memory_object_type(ptr);
            size_t read_size = object ? object->size : 0;
            return anvil_memory_alias(store_ptr, instr->operands[0]->type->size, ptr, read_size) != ANVIL_ALIAS_NO;
        }
    }
    
    /* Unknown calls remain full barriers, including external/volatile effects. */
    if (instr->op == ANVIL_OP_CALL) {
        unsigned barriers = ANVIL_EFFECT_WRITE_MEMORY | ANVIL_EFFECT_OBSERVABLE;
        return (anvil_opt_call_effects(instr) & barriers) != 0;
    }
    
    return false;
}

/* Check every incoming path between a dominating access and this load. A loop
 * is visited once; its writes still invalidate availability even when the
 * header itself contains no store. */
static bool paths_preserve_value(anvil_instr_t *load, anvil_instr_t *candidate, const anvil_opt_cfg_t *cfg, bool *visited, size_t *worklist)
{
    memset(visited, 0, cfg->count * sizeof(*visited));
    size_t original = anvil_opt_cfg_index(cfg, load->parent);
    size_t count = 1;
    worklist[0] = original;
    bool first = true;

    while (count)
    {
        size_t block = worklist[--count];
        /* A backedge may revisit the load's block. On that visit its suffix
         * must also be checked: a store after the load affects the next trip. */
        anvil_instr_t *cursor = first ? load->prev : cfg->blocks[block]->last;
        first = false;
        for (; cursor && cursor != candidate; cursor = cursor->prev)
        {
            if (cursor->memory_access.is_volatile || may_modify_ptr(cursor, load->operands[0]))
                return false;
        }
        if (cursor == candidate)
            continue;

        for (size_t edge = cfg->predecessor_offsets[block]; edge < cfg->predecessor_offsets[block + 1]; edge++)
        {
            size_t predecessor = cfg->predecessors[edge];
            if (!visited[predecessor] && cfg->rpo_rank[predecessor] != SIZE_MAX)
            {
                visited[predecessor] = true;
                worklist[count++] = predecessor;
            }
        }
    }

    return true;
}

static anvil_value_t *find_available_load(anvil_instr_t *load, const anvil_opt_cfg_t *cfg, bool *visited, size_t *worklist)
{
    if (!load || load->memory_access.is_volatile || !load->num_operands)
        return NULL;

    size_t block = anvil_opt_cfg_index(cfg, load->parent);
    if (block == SIZE_MAX || cfg->rpo_rank[block] == SIZE_MAX)
        return NULL;

    anvil_value_t *pointer = load->operands[0];
    anvil_instr_t *cursor = load->prev;
    for (;;)
    {
        for (anvil_instr_t *instr = cursor; instr; instr = instr->prev)
        {
            if (instr->memory_access.is_volatile)
                return NULL;

            anvil_value_t *value = NULL;
            anvil_value_t *address = NULL;
            if (instr->op == ANVIL_OP_LOAD && instr->num_operands)
            {
                value = instr->result;
                address = instr->operands[0];
            }
            else if (instr->op == ANVIL_OP_STORE && instr->num_operands == 2)
            {
                value = instr->operands[0];
                address = instr->operands[1];
            }

            if (value && anvil_types_equal(value->type, load->result->type) &&
                anvil_memory_alias(address, value->type->size, pointer, load->result->type->size) == ANVIL_ALIAS_MUST)
            {
                return paths_preserve_value(load, instr, cfg, visited, worklist) ? value : NULL;
            }
            if (may_modify_ptr(instr, pointer))
                return NULL;
        }

        size_t parent = cfg->idom[block];
        if (parent == SIZE_MAX || parent == block)
            return NULL;

        block = parent;
        cursor = cfg->blocks[block]->last;
    }
}

/* Main redundant load elimination pass */
anvil_pass_result_t anvil_pass_load_elim(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;

    anvil_opt_cfg_t cfg;
    if (!anvil_opt_cfg_build(func, &cfg))
        return ANVIL_PASS_RUN_ERROR;

    bool *visited = anvil_ctx_calloc(ctx, cfg.count, sizeof(*visited));
    size_t *worklist = anvil_ctx_calloc(ctx, cfg.count, sizeof(*worklist));
    if (!visited || !worklist)
    {
        free(visited);
        free(worklist);
        anvil_opt_cfg_destroy(&cfg);
        return ANVIL_PASS_RUN_ERROR;
    }
    
    bool changed = false;
    
    /* Iterate through all blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_LOAD) continue;
            
            /* Try to find an available load */
            anvil_value_t *available = find_available_load(instr, &cfg, visited, worklist);
            if (!available) continue;
            
            /* Replace uses of this load's result with the available value.
             * SSA invariant guarantees uses only occur in blocks dominated by
             * the load; the available value comes from a dominating access
             * whose memory value is preserved on every incoming path. */
            anvil_value_t *old_result = instr->result;
            if (old_result && anvil_opt_replace_uses_in_func(func, old_result, available) > 0) {
                anvil_opt_erase_instr(instr);
                changed = true;
            }
        }
    }
    
    free(visited);
    free(worklist);
    anvil_opt_cfg_destroy(&cfg);
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
