/*
 * ANVIL - Simplify CFG Pass
 * 
 * Simplifies the control flow graph:
 * - Removes unreachable blocks
 * - Merges blocks with single predecessor/successor
 * - Removes empty blocks (just a branch)
 * - Simplifies conditional branches with constant conditions
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Mark blocks reachable from entry */
static void mark_reachable(anvil_block_t *block, bool *reachable, size_t num_blocks)
{
    if (!block || block->id >= num_blocks || reachable[block->id]) return;
    
    reachable[block->id] = true;
    
    /* Find terminator and mark successors */
    anvil_instr_t *term = block->last;
    if (!term) return;
    
    if (term->op == ANVIL_OP_BR) {
        mark_reachable(term->true_block, reachable, num_blocks);
    } else if (term->op == ANVIL_OP_BR_COND) {
        mark_reachable(term->true_block, reachable, num_blocks);
        mark_reachable(term->false_block, reachable, num_blocks);
    }
}

/* Walk the function once and populate block->preds for every block. We
 * refresh this cache at the top of each do-while iteration; count_preds
 * below then runs in O(1) instead of O(n) per call. */
static void recompute_preds(anvil_func_t *func)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        block->num_preds = 0;
    }
    /* First pass: count to size the arrays. */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        if (term->op == ANVIL_OP_BR && term->true_block) {
            term->true_block->num_preds++;
        } else if (term->op == ANVIL_OP_BR_COND) {
            if (term->true_block)  term->true_block->num_preds++;
            if (term->false_block) term->false_block->num_preds++;
        }
    }
    /* Allocate per-block pred arrays. */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        free(block->preds);
        block->preds = block->num_preds
            ? calloc(block->num_preds, sizeof(anvil_block_t *))
            : NULL;
        block->num_preds = 0; /* re-use as write index in the next pass */
    }
    /* Second pass: fill. */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        if (term->op == ANVIL_OP_BR && term->true_block && term->true_block->preds) {
            term->true_block->preds[term->true_block->num_preds++] = block;
        } else if (term->op == ANVIL_OP_BR_COND) {
            if (term->true_block && term->true_block->preds)
                term->true_block->preds[term->true_block->num_preds++] = block;
            if (term->false_block && term->false_block->preds)
                term->false_block->preds[term->false_block->num_preds++] = block;
        }
    }
}

/* Count predecessors of a block (O(1) after recompute_preds). */
static size_t count_preds(anvil_func_t *func, anvil_block_t *target)
{
    (void)func;
    return target ? target->num_preds : 0;
}

/* Check if block has only one instruction (the terminator) */
static bool is_empty_block(anvil_block_t *block)
{
    return block->first == block->last && 
           block->first && 
           block->first->op == ANVIL_OP_BR;
}

/* Replace all branches to old_block with branches to new_block */
static void replace_branch_target(anvil_func_t *func, 
                                   anvil_block_t *old_block, 
                                   anvil_block_t *new_block)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        
        if (term->op == ANVIL_OP_BR) {
            if (term->true_block == old_block) {
                term->true_block = new_block;
            }
        } else if (term->op == ANVIL_OP_BR_COND) {
            if (term->true_block == old_block) {
                term->true_block = new_block;
            }
            if (term->false_block == old_block) {
                term->false_block = new_block;
            }
        }
    }
    
    /* Update PHI nodes in new_block */
    for (anvil_instr_t *instr = new_block->first; instr; instr = instr->next) {
        if (instr->op == ANVIL_OP_PHI) {
            for (size_t i = 0; i < instr->num_phi_incoming; i++) {
                if (instr->phi_blocks[i] == old_block) {
                    instr->phi_blocks[i] = new_block;
                }
            }
        }
    }
}

/* Remove a block from the function. Also refreshes func->last_block if the
 * removed block was at the tail. */
static void remove_block(anvil_func_t *func, anvil_block_t *block)
{
    anvil_block_t **pp = &func->blocks;
    while (*pp) {
        if (*pp == block) {
            *pp = block->next;
            func->num_blocks--;
            if (func->last_block == block) {
                anvil_block_t *new_last = func->blocks;
                if (new_last) {
                    while (new_last->next) new_last = new_last->next;
                }
                func->last_block = new_last;
            }
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Simplify conditional branch with constant condition */
static bool simplify_const_branch(anvil_func_t *func, anvil_block_t *block)
{
    anvil_instr_t *term = block->last;
    if (!term || term->op != ANVIL_OP_BR_COND) return false;
    if (term->num_operands < 1) return false;
    
    anvil_value_t *cond = term->operands[0];
    if (cond->kind != ANVIL_VAL_CONST_INT) return false;
    
    int64_t val = cond->data.i;
    anvil_block_t *target = val ? term->true_block : term->false_block;

    /* Convert to unconditional branch. Free the condition operand array
     * so the BR_COND's allocation doesn't leak. */
    term->op = ANVIL_OP_BR;
    term->true_block = target;
    term->false_block = NULL;
    free(term->operands);
    term->operands = NULL;
    term->num_operands = 0;

    return true;
}

/* Merge block with its single successor if possible */
static bool try_merge_blocks(anvil_func_t *func, anvil_block_t *block)
{
    anvil_instr_t *term = block->last;
    if (!term || term->op != ANVIL_OP_BR) return false;
    
    anvil_block_t *succ = term->true_block;
    if (!succ) return false;
    
    /* Don't merge entry block's successor if it has multiple preds */
    if (count_preds(func, succ) != 1) return false;
    
    /* Don't merge if successor has PHI nodes */
    if (succ->first && succ->first->op == ANVIL_OP_PHI) return false;
    
    /* Don't merge block with itself */
    if (block == succ) return false;
    
    /* Remove the branch instruction */
    if (term->prev) {
        term->prev->next = NULL;
        block->last = term->prev;
    } else {
        block->first = NULL;
        block->last = NULL;
    }
    
    /* Move all instructions from successor to this block */
    if (succ->first) {
        if (block->last) {
            block->last->next = succ->first;
            succ->first->prev = block->last;
        } else {
            block->first = succ->first;
        }
        block->last = succ->last;
        
        /* Update parent pointers */
        for (anvil_instr_t *instr = succ->first; instr; instr = instr->next) {
            instr->parent = block;
        }
    }
    
    /* Update branches to successor to point to this block */
    replace_branch_target(func, succ, block);
    
    /* Remove successor block */
    remove_block(func, succ);
    
    return true;
}

/* Simplify CFG pass */
bool anvil_pass_simplify_cfg(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    bool any_changed;
    
    do {
        any_changed = false;

        /* Refresh predecessor cache before any per-block queries. */
        recompute_preds(func);

        /* Simplify constant conditional branches */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            if (simplify_const_branch(func, block)) {
                any_changed = true;
                changed = true;
            }
        }
        
        /* Remove empty blocks (blocks with just a branch) */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            /* Don't remove entry block */
            if (block == func->entry) continue;
            
            if (is_empty_block(block)) {
                anvil_block_t *target = block->last->true_block;
                if (target && target != block) {
                    replace_branch_target(func, block, target);
                    /* Block will be removed as unreachable */
                    any_changed = true;
                    changed = true;
                }
            }
        }
        
        /* Try to merge blocks */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            if (try_merge_blocks(func, block)) {
                any_changed = true;
                changed = true;
                break;  /* Restart iteration after modification */
            }
        }
        
        /* Remove unreachable blocks */
        size_t max_id = 0;
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            if (block->id > max_id) max_id = block->id;
        }
        
        bool *reachable = calloc(max_id + 1, sizeof(bool));
        if (reachable) {
            mark_reachable(func->entry, reachable, max_id + 1);
            
            anvil_block_t *block = func->blocks;
            while (block) {
                anvil_block_t *next = block->next;
                if (!reachable[block->id] && block != func->entry) {
                    remove_block(func, block);
                    any_changed = true;
                    changed = true;
                }
                block = next;
            }
            
            free(reachable);
        }
        
    } while (any_changed);
    
    return changed;
}
