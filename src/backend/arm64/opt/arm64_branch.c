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
 * Pattern: CMP followed by BR_COND on the result
 * This pattern can potentially be optimized to use ARM64's
 * conditional branch instructions directly (b.eq, b.ne, etc.)
 * instead of materializing the boolean result.
 */
static void analyze_cmp_branch_pattern(anvil_func_t *func)
{
    if (!func) return;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (is_comparison(instr) && instr->result) {
                /* Check if result is only used by a BR_COND */
                /* This would be a candidate for optimization */
                /* For now, just identify the pattern */
            }
        }
    }
}

void arm64_opt_branch(arm64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func) return;
    
    /* Analyze comparison/branch patterns for potential optimization */
    analyze_cmp_branch_pattern(func);
    
    /* Note: The actual optimization of comparison/branch sequences
     * is now implemented in arm64_emit_br_cond() which:
     * 1. Detects when BR_COND condition is a comparison result
     * 2. Emits fused cmp + b.cond (or cbz/cbnz for zero comparisons)
     * 
     * This optimization works at the backend level, independent of
     * the frontend that generates the ANVIL IR.
     */
    
    (void)be;
}
