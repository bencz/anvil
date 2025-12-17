/*
 * ANVIL - ARM64 Branch/Comparison Optimization
 * 
 * Optimize comparison and branch sequences for ARM64.
 * 
 * The MCC code generator produces verbose comparison sequences like:
 *   cmp x9, x10
 *   cset x0, le
 *   strb w0, [stack]
 *   ldrsb x9, [stack]
 *   cmp x9, #0
 *   cset x0, ne
 *   cbnz x9, .body
 * 
 * This can be optimized to:
 *   cmp x9, x10
 *   b.le .body
 * 
 * However, this optimization requires changes at the IR level or during
 * code emission, not just peephole on the generated assembly.
 * 
 * For now, we mark patterns that could be optimized for future work.
 */

#include "arm64_opt.h"

/*
 * Check if instruction is a comparison that produces a boolean result
 */
static bool is_comparison(anvil_instr_t *instr)
{
    if (!instr) return false;
    
    switch (instr->op) {
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
        case ANVIL_OP_CMP_ULT:
        case ANVIL_OP_CMP_ULE:
        case ANVIL_OP_CMP_UGT:
        case ANVIL_OP_CMP_UGE:
            return true;
        default:
            return false;
    }
}

/*
 * Check if instruction is a conditional branch
 */
static bool is_cond_branch(anvil_instr_t *instr)
{
    return instr && instr->op == ANVIL_OP_BR_COND;
}

/*
 * Pattern: CMP followed by STORE of result, then BR_COND using the CMP result
 * 
 * Before:
 *   %cmp = CMP_LE %a, %b
 *   STORE %cmp -> %slot        ; unnecessary if %cmp only used by BR_COND
 *   BR_COND %cmp, true, false
 * 
 * After:
 *   %cmp = CMP_LE %a, %b
 *   BR_COND %cmp, true, false
 *   ; STORE converted to NOP
 */
static bool opt_cmp_store_branch(anvil_block_t *block)
{
    bool changed = false;
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (!is_comparison(instr) || !instr->result) continue;
        
        anvil_value_t *cmp_result = instr->result;
        
        /* Look for STORE of this comparison result followed by BR_COND */
        for (anvil_instr_t *next = instr->next; next; next = next->next) {
            if (next->op == ANVIL_OP_NOP) continue;
            
            /* If we hit a BR_COND that uses our cmp result, we can eliminate
             * any intermediate STOREs of the cmp result */
            if (next->op == ANVIL_OP_BR_COND) {
                if (next->num_operands >= 1 && next->operands[0] == cmp_result) {
                    /* Found BR_COND using our comparison result directly.
                     * Now go back and NOP any STOREs of this result. */
                    for (anvil_instr_t *s = instr->next; s != next; s = s->next) {
                        if (s->op == ANVIL_OP_STORE && s->num_operands >= 1 &&
                            s->operands[0] == cmp_result) {
                            s->op = ANVIL_OP_NOP;
                            changed = true;
                        }
                    }
                }
                break;  /* Stop at branch */
            }
            
            /* If we hit another use of cmp_result (not STORE), stop */
            if (next->op != ANVIL_OP_STORE) {
                bool uses_cmp = false;
                for (size_t i = 0; i < next->num_operands; i++) {
                    if (next->operands[i] == cmp_result) {
                        uses_cmp = true;
                        break;
                    }
                }
                if (uses_cmp) break;
            }
        }
    }
    
    return changed;
}

/*
 * Pattern: LOAD followed immediately by STORE to different location
 * where the LOAD result is only used by that STORE.
 * This is a copy operation that may be eliminable.
 */
static bool opt_redundant_load_for_store(anvil_block_t *block)
{
    bool changed = false;
    
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (instr->op != ANVIL_OP_LOAD || !instr->result) continue;
        
        anvil_instr_t *next = instr->next;
        while (next && next->op == ANVIL_OP_NOP) next = next->next;
        
        if (!next || next->op != ANVIL_OP_STORE) continue;
        
        /* Check if STORE uses the LOAD result as its value */
        if (next->num_operands >= 1 && next->operands[0] == instr->result) {
            /* Check if LOAD result is used anywhere else */
            bool used_elsewhere = false;
            for (anvil_instr_t *check = next->next; check; check = check->next) {
                for (size_t i = 0; i < check->num_operands; i++) {
                    if (check->operands[i] == instr->result) {
                        used_elsewhere = true;
                        break;
                    }
                }
                if (used_elsewhere) break;
            }
            
            /* If LOAD result only used by this STORE, and they're to/from
             * the same address, eliminate both */
            if (!used_elsewhere && instr->num_operands >= 1 && next->num_operands >= 2) {
                if (instr->operands[0] == next->operands[1]) {
                    /* LOAD from X, STORE to X - no-op */
                    instr->op = ANVIL_OP_NOP;
                    next->op = ANVIL_OP_NOP;
                    changed = true;
                }
            }
        }
    }
    
    return changed;
}

void arm64_opt_branch(arm64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func) return;
    
    bool changed = true;
    int iterations = 0;
    const int max_iterations = 5;
    
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            /* Optimize comparison + store + branch patterns */
            if (opt_cmp_store_branch(block)) changed = true;
            
            /* Optimize redundant load-store pairs */
            if (opt_redundant_load_for_store(block)) changed = true;
        }
    }
    
    /* Note: Additional optimization of comparison/branch sequences
     * is implemented in arm64_emit_br_cond() which:
     * 1. Detects when BR_COND condition is a comparison result
     * 2. Emits fused cmp + b.cond (or cbz/cbnz for zero comparisons)
     * 
     * This optimization works at the backend level, independent of
     * the frontend that generates the ANVIL IR.
     */
    
    (void)be;
}
