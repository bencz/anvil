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

/* Count predecessors of a block */
static size_t count_preds(anvil_func_t *func, anvil_block_t *target)
{
    size_t count = 0;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        
        if (term->op == ANVIL_OP_BR && term->true_block == target) {
            count++;
        } else if (term->op == ANVIL_OP_BR_COND) {
            if (term->true_block == target) count++;
            if (term->false_block == target) count++;
        }
    }
    
    return count;
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

/* Remove a block from the function */
static void remove_block(anvil_func_t *func, anvil_block_t *block)
{
    anvil_block_t **pp = &func->blocks;
    while (*pp) {
        if (*pp == block) {
            *pp = block->next;
            func->num_blocks--;
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
    
    /* Convert to unconditional branch */
    term->op = ANVIL_OP_BR;
    term->true_block = target;
    term->false_block = NULL;
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
