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

/* Replace all uses of old_val with new_val starting from instruction */
static int replace_uses(anvil_value_t *old_val, anvil_value_t *new_val, anvil_instr_t *start)
{
    int count = 0;
    for (anvil_instr_t *i = start; i; i = i->next) {
        if (i->op == ANVIL_OP_NOP) continue;
        for (size_t j = 0; j < i->num_operands; j++) {
            if (i->operands[j] == old_val) {
                i->operands[j] = new_val;
                count++;
            }
        }
    }
    return count;
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
    if (store->num_operands < 2 || load->num_operands < 1) return false;
    if (!load->result) return false;
    
    /* Check if loading from same address we just stored to */
    if (!values_equal(store->operands[1], load->operands[0])) return false;
    
    /* Replace all uses of load result with the stored value */
    anvil_value_t *stored_val = store->operands[0];
    anvil_value_t *load_result = load->result;
    
    int replaced = replace_uses(load_result, stored_val, load->next);
    
    if (replaced > 0) {
        /* Eliminate the load */
        load->op = ANVIL_OP_NOP;
        return true;
    }
    
    return false;
}

/* Main store-load propagation pass */
bool anvil_pass_store_load_prop(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    bool any_changed;
    int iterations = 0;
    const int max_iterations = 10;
    
    do {
        any_changed = false;
        iterations++;
        
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
                if (instr->op == ANVIL_OP_NOP) continue;
                if (instr->op != ANVIL_OP_STORE) continue;
                
                /* Find next non-NOP instruction */
                anvil_instr_t *next = instr->next;
                while (next && next->op == ANVIL_OP_NOP) next = next->next;
                
                if (next && opt_store_load_propagate(instr, next)) {
                    any_changed = true;
                    changed = true;
                }
            }
        }
    } while (any_changed && iterations < max_iterations);
    
    return changed;
}
