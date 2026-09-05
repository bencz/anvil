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
#include "opt_utils.h"
#include <stdlib.h>
#include <string.h>

#define same_pointer anvil_opt_same_pointer

/* Check if an instruction may observe the value written by a store.
 *
 * Without alias analysis, a load through a different SSA pointer is not proof
 * that the locations are disjoint: function parameters, GEPs and bitcasts may
 * still alias.  A later overwrite may kill the store only after every
 * intervening potential read has been ruled out, so conservatively treat every
 * load and call as a read barrier. */
static bool may_read_memory(anvil_instr_t *instr)
{
    if (!instr) return false;
    if (anvil_op_is_atomic(instr->op))
        return true;

    if (instr->op == ANVIL_OP_CALL)
    {
        unsigned barriers = ANVIL_EFFECT_READ_MEMORY | ANVIL_EFFECT_MAY_TRAP | ANVIL_EFFECT_MAY_UNWIND |
                            ANVIL_EFFECT_MAY_NOT_RETURN | ANVIL_EFFECT_OBSERVABLE;
        return (anvil_opt_call_effects(instr) & barriers) != 0;
    }

    return instr->op == ANVIL_OP_LOAD;
}

static bool may_exit_before_overwrite(const anvil_instr_t *instr)
{
    switch (instr->op)
    {
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
        case ANVIL_OP_SMOD:
        case ANVIL_OP_UMOD:
        case ANVIL_OP_STORE:
        case ANVIL_OP_ALLOCA:
        case ANVIL_OP_FADD:
        case ANVIL_OP_FSUB:
        case ANVIL_OP_FMUL:
        case ANVIL_OP_FDIV:
        case ANVIL_OP_FCMP:
        case ANVIL_OP_FPTRUNC:
        case ANVIL_OP_FPEXT:
        case ANVIL_OP_FPTOSI:
        case ANVIL_OP_FPTOUI:
        case ANVIL_OP_SITOFP:
        case ANVIL_OP_UITOFP:
            return true;
        default:
            return false;
    }
}

/* Check if a store is dead (overwritten before read) within the same block */
static bool is_dead_store(anvil_instr_t *store)
{
    if (!store || store->op != ANVIL_OP_STORE) return false;
    if (store->memory_access.is_volatile)
        return false;
    if (store->num_operands < 2) return false;
    
    anvil_value_t *ptr = store->operands[1];
    
    /* Look at subsequent instructions in the same block */
    for (anvil_instr_t *instr = store->next; instr; instr = instr->next) {
        if (instr->memory_access.is_volatile)
            return false;

        /* Any potentially aliasing read observes the first store. */
        if (may_read_memory(instr)) {
            return false;
        }
        
        /* If we write to this pointer again, original store is dead */
        if (instr->op == ANVIL_OP_STORE && 
            instr->num_operands > 1 && 
            same_pointer(instr->operands[1], ptr)) {
            return true;
        }

        /* A handler reached before the overwrite may observe the first store. */
        if (may_exit_before_overwrite(instr))
            return false;
        
        /* If we hit a branch, stop (cross-block analysis not done here) */
        if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND ||
            instr->op == ANVIL_OP_RET) {
            return false;
        }
    }
    
    return false;
}

/* Main dead store elimination pass */
anvil_pass_result_t anvil_pass_dead_store(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;
    
    bool changed = false;
    
    /* Iterate through all blocks */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *instr = block->first;
        
        while (instr) {
            anvil_instr_t *next = instr->next;
            
            if (instr->op == ANVIL_OP_STORE && is_dead_store(instr)) {
                anvil_opt_erase_instr(instr);
                changed = true;
            }
            
            instr = next;
        }
    }
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
