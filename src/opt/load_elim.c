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
#include <stdlib.h>
#include <string.h>

/* Check if two pointer values are definitely the same */
static bool same_pointer(anvil_value_t *p1, anvil_value_t *p2)
{
    if (!p1 || !p2) return false;
    if (p1 == p2) return true;
    
    /* Same alloca result */
    if (p1->kind == ANVIL_VAL_INSTR && p2->kind == ANVIL_VAL_INSTR) {
        anvil_instr_t *i1 = p1->data.instr;
        anvil_instr_t *i2 = p2->data.instr;
        if (i1 == i2) return true;
    }
    
    return false;
}

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

/* Replace all uses of old_val with new_val in the function */
static int replace_uses_after(anvil_instr_t *start, anvil_value_t *old_val, anvil_value_t *new_val)
{
    int count = 0;
    
    /* Replace in remaining instructions of this block */
    for (anvil_instr_t *instr = start->next; instr; instr = instr->next) {
        for (size_t i = 0; i < instr->num_operands; i++) {
            if (instr->operands[i] == old_val) {
                instr->operands[i] = new_val;
                count++;
            }
        }
    }
    
    /* Replace in subsequent blocks */
    anvil_block_t *block = start->parent;
    if (block) {
        for (anvil_block_t *b = block->next; b; b = b->next) {
            for (anvil_instr_t *instr = b->first; instr; instr = instr->next) {
                for (size_t i = 0; i < instr->num_operands; i++) {
                    if (instr->operands[i] == old_val) {
                        instr->operands[i] = new_val;
                        count++;
                    }
                }
            }
        }
    }
    
    return count;
}

/* Main redundant load elimination pass */
bool anvil_pass_load_elim(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    
    /* Iterate through all blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_LOAD) continue;
            
            /* Try to find an available load */
            anvil_value_t *available = find_available_load(instr);
            if (!available) continue;
            
            /* Replace uses of this load's result with the available value */
            anvil_value_t *old_result = instr->result;
            if (old_result && replace_uses_after(instr, old_result, available) > 0) {
                /* Mark the redundant load as NOP */
                instr->op = ANVIL_OP_NOP;
                changed = true;
            }
        }
    }
    
    return changed;
}
