/*
 * ANVIL - Dead Store Elimination Pass
 * 
 * Removes store instructions that are overwritten before being read.
 * 
 * Example:
 *   *p = 1
 *   *p = 2
 * Becomes:
 *   *p = 2
 * 
 * The first store is dead because its value is never read.
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

/* Check if instruction may read from pointer p */
static bool may_read_ptr(anvil_instr_t *instr, anvil_value_t *ptr)
{
    if (!instr) return false;
    
    /* Load from same pointer */
    if (instr->op == ANVIL_OP_LOAD) {
        if (instr->num_operands > 0 && same_pointer(instr->operands[0], ptr)) {
            return true;
        }
    }
    
    /* Call may read any memory */
    if (instr->op == ANVIL_OP_CALL) {
        return true;
    }
    
    return false;
}

/* Check if instruction may write to pointer p (used for future extensions) */
static bool may_write_ptr(anvil_instr_t *instr, anvil_value_t *ptr)
    __attribute__((unused));

static bool may_write_ptr(anvil_instr_t *instr, anvil_value_t *ptr)
{
    if (!instr) return false;
    
    /* Store to same pointer */
    if (instr->op == ANVIL_OP_STORE) {
        if (instr->num_operands > 1 && same_pointer(instr->operands[1], ptr)) {
            return true;
        }
    }
    
    /* Call may write any memory */
    if (instr->op == ANVIL_OP_CALL) {
        return true;
    }
    
    return false;
}

/* Check if a store is dead (overwritten before read) within the same block */
static bool is_dead_store(anvil_instr_t *store)
{
    if (!store || store->op != ANVIL_OP_STORE) return false;
    if (store->num_operands < 2) return false;
    
    anvil_value_t *ptr = store->operands[1];
    
    /* Look at subsequent instructions in the same block */
    for (anvil_instr_t *instr = store->next; instr; instr = instr->next) {
        /* If we read from this pointer, store is not dead */
        if (may_read_ptr(instr, ptr)) {
            return false;
        }
        
        /* If we write to this pointer again, original store is dead */
        if (instr->op == ANVIL_OP_STORE && 
            instr->num_operands > 1 && 
            same_pointer(instr->operands[1], ptr)) {
            return true;
        }
        
        /* If we hit a call, be conservative */
        if (instr->op == ANVIL_OP_CALL) {
            return false;
        }
        
        /* If we hit a branch, stop (cross-block analysis not done here) */
        if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND ||
            instr->op == ANVIL_OP_RET) {
            return false;
        }
    }
    
    return false;
}

/* Main dead store elimination pass */
bool anvil_pass_dead_store(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    
    /* Iterate through all blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *instr = block->first;
        
        while (instr) {
            anvil_instr_t *next = instr->next;
            
            if (instr->op == ANVIL_OP_STORE && is_dead_store(instr)) {
                /* Mark as NOP (will be cleaned by DCE) */
                instr->op = ANVIL_OP_NOP;
                changed = true;
            }
            
            instr = next;
        }
    }
    
    return changed;
}
