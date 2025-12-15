/*
 * ANVIL - Dead Code Elimination Pass
 * 
 * Removes instructions whose results are never used.
 * Also removes NOP instructions left by other passes.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Check if an instruction has side effects (cannot be removed even if unused) */
static bool has_side_effects(anvil_instr_t *instr)
{
    switch (instr->op) {
        case ANVIL_OP_STORE:
        case ANVIL_OP_CALL:
        case ANVIL_OP_BR:
        case ANVIL_OP_BR_COND:
        case ANVIL_OP_RET:
        case ANVIL_OP_SWITCH:
            return true;
        default:
            return false;
    }
}

/* Check if a value is used anywhere in the function */
static bool is_value_used(anvil_func_t *func, anvil_value_t *val)
{
    if (!val) return false;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            
            for (size_t i = 0; i < instr->num_operands; i++) {
                if (instr->operands[i] == val) {
                    return true;
                }
            }
            
            /* Check PHI incoming values */
            if (instr->op == ANVIL_OP_PHI) {
                for (size_t i = 0; i < instr->num_phi_incoming; i++) {
                    if (instr->operands[i] == val) {
                        return true;
                    }
                }
            }
        }
    }
    
    return false;
}

/* Remove an instruction from its block */
static void remove_instr(anvil_instr_t *instr)
{
    anvil_block_t *block = instr->parent;
    if (!block) return;
    
    /* Unlink from list */
    if (instr->prev) {
        instr->prev->next = instr->next;
    } else {
        block->first = instr->next;
    }
    
    if (instr->next) {
        instr->next->prev = instr->prev;
    } else {
        block->last = instr->prev;
    }
}

/* Dead code elimination pass */
bool anvil_pass_dce(anvil_func_t *func)
{
    if (!func) return false;
    
    bool changed = false;
    bool any_removed;
    
    /* Iterate until no more dead code is found */
    do {
        any_removed = false;
        
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            anvil_instr_t *instr = block->first;
            
            while (instr) {
                anvil_instr_t *next = instr->next;
                
                /* Remove NOP instructions */
                if (instr->op == ANVIL_OP_NOP) {
                    remove_instr(instr);
                    any_removed = true;
                    changed = true;
                    instr = next;
                    continue;
                }
                
                /* Skip instructions with side effects */
                if (has_side_effects(instr)) {
                    instr = next;
                    continue;
                }
                
                /* Skip instructions without results */
                if (!instr->result) {
                    instr = next;
                    continue;
                }
                
                /* Remove if result is not used */
                if (!is_value_used(func, instr->result)) {
                    remove_instr(instr);
                    any_removed = true;
                    changed = true;
                }
                
                instr = next;
            }
        }
    } while (any_removed);
    
    return changed;
}
