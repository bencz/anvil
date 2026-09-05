/*
 * ANVIL - Store-Load Propagation Pass
 * 
 * Replaces loads that immediately follow stores to the same address
 * with the stored value, eliminating redundant memory accesses.
 * 
 * Example:
 *   store %val, %addr
 *   %x = load %addr
 * Becomes:
 *   store %val, %addr
 *   ; load eliminated, uses of %x replaced with %val
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include "opt_utils.h"
#include <stdlib.h>
#include <string.h>

/* Check if two values are the same */
static bool values_equal(anvil_value_t *a, anvil_value_t *b)
{
    if (!a || !b) return false;
    if (a == b) return true;
    
    /* Check if both are the same constant */
    if (a->kind == ANVIL_VAL_CONST_INT && b->kind == ANVIL_VAL_CONST_INT) {
        return a->data.i == b->data.i;
    }
    
    return false;
}

/*
 * Pattern: STORE followed by LOAD from same address
 * STORE %val -> %addr
 * LOAD %addr -> %result
 * Replace all uses of %result with %val and eliminate the LOAD
 */
static bool opt_store_load_propagate(anvil_instr_t *store, anvil_instr_t *load)
{
    if (!store || !load) return false;
    
    if (store->op != ANVIL_OP_STORE || load->op != ANVIL_OP_LOAD) return false;
    if (store->memory_access.is_volatile || load->memory_access.is_volatile)
        return false;
    if (store->num_operands < 2 || load->num_operands < 1) return false;
    if (!load->result) return false;
    
    /* Check if loading from same address we just stored to */
    if (!values_equal(store->operands[1], load->operands[0])) return false;
    
    /* Replace all uses of load result with the stored value */
    anvil_value_t *stored_val = store->operands[0];
    anvil_value_t *load_result = load->result;
    
    anvil_func_t *func = load->parent ? load->parent->parent : NULL;
    int replaced = anvil_opt_replace_uses_in_func(func, load_result, stored_val);

    if (replaced > 0) {
        /* Eliminate the load while preserving valid IR immediately. */
        anvil_opt_erase_instr(load);
        return true;
    }
    
    return false;
}

/* Main store-load propagation pass.
 * The pass manager runs a global fixpoint across all passes, so we only do one
 * sweep here — the former internal do-while loop duplicated that work. */
anvil_pass_result_t anvil_pass_store_load_prop(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;

    bool changed = false;

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            if (instr->op != ANVIL_OP_STORE) continue;

            /* Find next non-NOP instruction */
            anvil_instr_t *next = instr->next;
            while (next && next->op == ANVIL_OP_NOP) next = next->next;

            if (next && opt_store_load_propagate(instr, next)) {
                changed = true;
            }
        }
    }

    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
