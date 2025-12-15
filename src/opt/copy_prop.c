/*
 * ANVIL - Copy Propagation Pass
 * 
 * Replaces uses of copied values with the original value.
 * This eliminates unnecessary copy operations and enables
 * further optimizations.
 * 
 * Example:
 *   y = x
 *   z = y + 1
 * Becomes:
 *   y = x
 *   z = x + 1
 * 
 * The dead copy (y = x) can then be removed by DCE.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Check if instruction is a simple copy (LR in terms of codegen) */
static bool is_copy_instr(anvil_instr_t *instr)
{
    if (!instr || !instr->result) return false;
    
    /* A copy is an ADD with zero, or could be detected from other patterns */
    /* For now, we look for patterns that are effectively copies:
     * - ADD x, 0 -> copy of x
     * - SUB x, 0 -> copy of x  
     * - OR x, 0 -> copy of x
     * - AND x, -1 -> copy of x
     * - XOR x, 0 -> copy of x
     */
    if (instr->num_operands < 2) return false;
    
    anvil_value_t *op1 = instr->operands[1];
    if (!op1 || op1->kind != ANVIL_VAL_CONST_INT) return false;
    
    int64_t val = op1->data.i;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
        case ANVIL_OP_SUB:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            return val == 0;
        case ANVIL_OP_AND:
            return val == -1 || val == (int64_t)0xFFFFFFFF || val == (int64_t)0xFFFFFFFFFFFFFFFFULL;
        case ANVIL_OP_MUL:
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            return val == 1;
        default:
            return false;
    }
}

/* Get the source value of a copy instruction */
static anvil_value_t *get_copy_source(anvil_instr_t *instr)
{
    if (!is_copy_instr(instr)) return NULL;
    return instr->operands[0];
}

/* Replace all uses of old_val with new_val in the function */
static int replace_uses(anvil_func_t *func, anvil_value_t *old_val, anvil_value_t *new_val)
{
    int count = 0;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            for (size_t i = 0; i < instr->num_operands; i++) {
                if (instr->operands[i] == old_val) {
                    instr->operands[i] = new_val;
                    count++;
                }
            }
        }
    }
    
    return count;
}

/* Main copy propagation pass */
bool anvil_pass_copy_prop(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    
    /* Iterate through all instructions looking for copies */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (!is_copy_instr(instr)) continue;
            
            anvil_value_t *src = get_copy_source(instr);
            anvil_value_t *dst = instr->result;
            
            if (!src || !dst) continue;
            if (src == dst) continue;  /* Self-copy, skip */
            
            /* Replace all uses of dst with src */
            int replaced = replace_uses(func, dst, src);
            if (replaced > 0) {
                changed = true;
            }
        }
    }
    
    return changed;
}
