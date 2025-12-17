/*
 * ANVIL - ARM64 Peephole Optimizations
 * 
 * Local optimizations that look at small windows of instructions
 * to find and eliminate inefficiencies.
 */

#include "arm64_opt.h"
#include <string.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

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

/* ============================================================================
 * Peephole Patterns
 * ============================================================================ */

/*
 * Pattern: Consecutive STORE to same address - keep only last
 */
static bool opt_redundant_store(anvil_instr_t *instr, anvil_instr_t *next)
{
    if (!instr || !next) return false;
    
    if (instr->op == ANVIL_OP_STORE && next->op == ANVIL_OP_STORE) {
        /* Check if storing to same address */
        if (instr->num_operands >= 2 && next->num_operands >= 2) {
            if (values_equal(instr->operands[1], next->operands[1])) {
                /* Mark first store as dead by converting to NOP */
                instr->op = ANVIL_OP_NOP;
                return true;
            }
        }
    }
    
    return false;
}

/*
 * Pattern: LOAD followed by STORE to same address with no intervening use
 */
static bool opt_load_store_same(anvil_instr_t *instr, anvil_instr_t *next)
{
    if (!instr || !next) return false;
    
    if (instr->op == ANVIL_OP_LOAD && next->op == ANVIL_OP_STORE) {
        /* Check if load result is stored back to same address */
        if (instr->num_operands >= 1 && next->num_operands >= 2) {
            if (values_equal(instr->operands[0], next->operands[1]) &&
                values_equal(instr->result, next->operands[0])) {
                /* This is a no-op: load from X, store to X */
                instr->op = ANVIL_OP_NOP;
                next->op = ANVIL_OP_NOP;
                return true;
            }
        }
    }
    
    return false;
}

/* ============================================================================
 * Main Peephole Pass
 * ============================================================================ */

void arm64_opt_peephole(arm64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func) return;
    
    bool changed = true;
    int iterations = 0;
    const int max_iterations = 10;
    
    /* Iterate until no more changes or max iterations */
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
                /* Skip NOP instructions */
                if (instr->op == ANVIL_OP_NOP) continue;
                
                /* Try two-instruction patterns */
                anvil_instr_t *next = instr->next;
                if (next && next->op != ANVIL_OP_NOP) {
                    if (opt_redundant_store(instr, next)) {
                        changed = true;
                        continue;
                    }
                    
                    if (opt_load_store_same(instr, next)) {
                        changed = true;
                        continue;
                    }
                }
            }
        }
    }
    
    (void)be;  /* Suppress unused parameter warning */
}
