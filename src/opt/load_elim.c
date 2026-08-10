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
#include <stdlib.h>
#include <string.h>

#define same_pointer anvil_opt_same_pointer

/* Check if instruction may modify memory at pointer p */
static bool may_modify_ptr(anvil_instr_t *instr, anvil_value_t *ptr)
{
    if (!instr) return false;
    
    /* Store to same pointer */
    if (instr->op == ANVIL_OP_STORE) {
        if (instr->num_operands > 1) {
            /* If we can prove it's a different pointer, it's safe */
            /* For now, be conservative: any store might alias */
            anvil_value_t *store_ptr = instr->operands[1];
            if (same_pointer(store_ptr, ptr)) {
                return true;
            }
            /* If pointers are from different allocas, they don't alias */
            if (ptr->kind == ANVIL_VAL_INSTR && store_ptr->kind == ANVIL_VAL_INSTR) {
                anvil_instr_t *pi = ptr->data.instr;
                anvil_instr_t *si = store_ptr->data.instr;
                if (pi->op == ANVIL_OP_ALLOCA && si->op == ANVIL_OP_ALLOCA && pi != si) {
                    return false;  /* Different allocas don't alias */
                }
            }
            /* Conservative: assume aliasing */
            return true;
        }
    }
    
    /* Call may modify any memory */
    if (instr->op == ANVIL_OP_CALL) {
        return true;
    }
    
    return false;
}

/* Find a previous load from the same pointer that's still valid */
static anvil_value_t *find_available_load(anvil_instr_t *load_instr)
{
    if (!load_instr || load_instr->op != ANVIL_OP_LOAD) return NULL;
    if (load_instr->num_operands < 1) return NULL;
    
    anvil_value_t *ptr = load_instr->operands[0];
    
    /* Search backwards in the same block */
    for (anvil_instr_t *instr = load_instr->prev; instr; instr = instr->prev) {
        /* Found a previous load from same pointer */
        if (instr->op == ANVIL_OP_LOAD && 
            instr->num_operands > 0 &&
            same_pointer(instr->operands[0], ptr)) {
            return instr->result;
        }
        
        /* Memory may have been modified */
        if (may_modify_ptr(instr, ptr)) {
            return NULL;
        }
    }
    
    return NULL;
}

/* Main redundant load elimination pass */
anvil_pass_result_t anvil_pass_load_elim(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;
    
    bool changed = false;
    
    /* Iterate through all blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_LOAD) continue;
            
            /* Try to find an available load */
            anvil_value_t *available = find_available_load(instr);
            if (!available) continue;
            
            /* Replace uses of this load's result with the available value.
             * SSA invariant guarantees uses only occur in blocks dominated by
             * `instr`; `available` comes from an earlier load in the same
             * block as `instr` so it dominates all those uses too. */
            anvil_value_t *old_result = instr->result;
            if (old_result && anvil_opt_replace_uses_in_func(func, old_result, available) > 0) {
                anvil_opt_erase_instr(instr);
                changed = true;
            }
        }
    }
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
