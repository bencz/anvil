/*
 * ANVIL - Dead Code Elimination Pass
 *
 * Removes instructions whose results are never used, plus NOPs left by other
 * passes.
 *
 * Algorithm: build an array of per-value use counts keyed by value->id, then
 * sweep the function. Each removal decrements its operands' counts, which
 * can make further instructions dead — repeat until a sweep removes nothing.
 *
 * Previously this was O(n²) per sweep (is_value_used walked the entire
 * function for every candidate) and the outer fixpoint made it O(n³) in the
 * worst case. Now it's O(n) per sweep, and the number of sweeps is bounded
 * by the longest chain of strictly-linear dependencies (in practice single
 * digits).
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

/* Remove an instruction from its block */
static void remove_instr(anvil_instr_t *instr)
{
    anvil_block_t *block = instr->parent;
    if (!block) return;

    if (instr->prev) instr->prev->next = instr->next;
    else             block->first = instr->next;

    if (instr->next) instr->next->prev = instr->prev;
    else             block->last = instr->prev;
}

/* Populate `use_counts` with the number of times each value id appears as an
 * operand (PHI incoming values are part of num_operands). */
static void build_use_counts(anvil_func_t *func, int *use_counts, uint32_t cap)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_NOP) continue;
            for (size_t i = 0; i < instr->num_operands; i++) {
                anvil_value_t *op = instr->operands[i];
                if (op && op->id < cap) {
                    use_counts[op->id]++;
                }
            }
        }
    }
}

/* Decrement use counts for each operand of `instr`. */
static void decrement_operand_uses(anvil_instr_t *instr, int *use_counts, uint32_t cap)
{
    for (size_t i = 0; i < instr->num_operands; i++) {
        anvil_value_t *op = instr->operands[i];
        if (op && op->id < cap && use_counts[op->id] > 0) {
            use_counts[op->id]--;
        }
    }
}

bool anvil_pass_dce(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx) return false;

    anvil_ctx_t *ctx = func->parent->ctx;
    uint32_t cap = ctx->next_value_id + 1;

    int *use_counts = calloc(cap, sizeof(int));
    if (!use_counts) return false;

    build_use_counts(func, use_counts, cap);

    bool changed = false;
    bool any_removed;

    do {
        any_removed = false;

        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            anvil_instr_t *instr = block->first;
            while (instr) {
                anvil_instr_t *next = instr->next;

                if (instr->op == ANVIL_OP_NOP) {
                    remove_instr(instr);
                    any_removed = true;
                    changed = true;
                    instr = next;
                    continue;
                }

                if (has_side_effects(instr) || !instr->result) {
                    instr = next;
                    continue;
                }

                if (instr->result->id < cap &&
                    use_counts[instr->result->id] == 0) {
                    decrement_operand_uses(instr, use_counts, cap);
                    remove_instr(instr);
                    any_removed = true;
                    changed = true;
                }

                instr = next;
            }
        }
    } while (any_removed);

    free(use_counts);
    return changed;
}
