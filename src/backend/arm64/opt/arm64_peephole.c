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

/*
 * Check if a value is used after a given instruction
 */
static bool value_used_after(anvil_value_t *val, anvil_instr_t *after)
{
    if (!val || !after) return false;
    
    /* Check remaining instructions in this block */
    for (anvil_instr_t *i = after->next; i; i = i->next) {
        if (i->op == ANVIL_OP_NOP) continue;
        for (size_t j = 0; j < i->num_operands; j++) {
            if (i->operands[j] == val) return true;
        }
    }
    
    /* For simplicity, assume value might be used in other blocks */
    /* A more complete analysis would check all blocks */
    return true;
}

/*
 * Pattern: STORE followed by LOAD from same address
 * STORE %val -> %addr
 * LOAD %addr -> %result
 * If %result is only used once immediately after, we can propagate %val
 */
static bool opt_store_load_propagate(anvil_instr_t *store, anvil_instr_t *load)
{
    if (!store || !load) return false;
    
    if (store->op != ANVIL_OP_STORE || load->op != ANVIL_OP_LOAD) return false;
    if (store->num_operands < 2 || load->num_operands < 1) return false;
    
    /* Check if loading from same address we just stored to */
    if (!values_equal(store->operands[1], load->operands[0])) return false;
    
    /* Check if load result is only used in the next instruction */
    anvil_instr_t *use = load->next;
    while (use && use->op == ANVIL_OP_NOP) use = use->next;
    
    if (!use) return false;
    
    /* Check if load result is used in the next instruction */
    bool found_use = false;
    for (size_t i = 0; i < use->num_operands; i++) {
        if (use->operands[i] == load->result) {
            /* Replace with the original stored value */
            use->operands[i] = store->operands[0];
            found_use = true;
        }
    }
    
    if (found_use && !value_used_after(load->result, use)) {
        /* Can eliminate the load */
        load->op = ANVIL_OP_NOP;
        return true;
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
                while (next && next->op == ANVIL_OP_NOP) next = next->next;
                
                if (next) {
                    if (opt_redundant_store(instr, next)) {
                        changed = true;
                        continue;
                    }
                    
                    if (opt_load_store_same(instr, next)) {
                        changed = true;
                        continue;
                    }
                    
                    if (opt_store_load_propagate(instr, next)) {
                        changed = true;
                        continue;
                    }
                }
            }
        }
    }
    
    (void)be;  /* Suppress unused parameter warning */
}
