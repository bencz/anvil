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

typedef struct {
    anvil_ctx_t *ctx;
    anvil_block_t **blocks;
    size_t count;
    anvil_block_t **hash_keys;
    size_t *hash_values;
    size_t hash_cap;
} block_index_t;

static size_t ptr_hash(const void *ptr)
{
    uintptr_t value = (uintptr_t)ptr;
    value ^= value >> 17;
    value *= UINT64_C(0xed5ad4bb);
    value ^= value >> 11;
    return (size_t)value;
}

static void block_index_destroy(block_index_t *index)
{
    if (!index) return;
    free(index->blocks);
    free(index->hash_keys);
    free(index->hash_values);
    memset(index, 0, sizeof(*index));
}

static bool block_index_build(anvil_func_t *func, block_index_t *index)
{
    memset(index, 0, sizeof(*index));
    index->ctx = func->parent->ctx;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (index->count == SIZE_MAX) {
            anvil_set_error(index->ctx, ANVIL_ERR_NOMEM,
                            "CFG block count overflow");
            return false;
        }
        index->count++;
    }
    if (index->count == 0) return true;
    index->blocks = anvil_ctx_malloc(
        index->ctx, index->count * sizeof(*index->blocks));
    if (!index->blocks) return false;

    if (index->count > SIZE_MAX / 2) {
        anvil_set_error(index->ctx, ANVIL_ERR_NOMEM,
                        "CFG block index capacity overflow");
        block_index_destroy(index);
        return false;
    }
    size_t required = index->count * 2;
    size_t cap = 1;
    while (cap < required) {
        if (cap > SIZE_MAX / 2) {
            anvil_set_error(index->ctx, ANVIL_ERR_NOMEM,
                            "CFG block index capacity overflow");
            block_index_destroy(index);
            return false;
        }
        cap *= 2;
    }
    index->hash_keys = anvil_ctx_calloc(
        index->ctx, cap, sizeof(*index->hash_keys));
    index->hash_values = anvil_ctx_malloc(
        index->ctx, cap * sizeof(*index->hash_values));
    if (!index->hash_keys || !index->hash_values) {
        block_index_destroy(index);
        return false;
    }
    index->hash_cap = cap;

    size_t i = 0;
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        index->blocks[i] = block;
        size_t slot = ptr_hash(block) & (cap - 1);
        while (index->hash_keys[slot]) slot = (slot + 1) & (cap - 1);
        index->hash_keys[slot] = block;
        index->hash_values[slot] = i++;
    }
    return true;
}

static bool block_index_find(const block_index_t *index,
                             anvil_block_t *block, size_t *result)
{
    if (!index || !block || index->hash_cap == 0) return false;
    size_t slot = ptr_hash(block) & (index->hash_cap - 1);
    for (size_t probe = 0; probe < index->hash_cap; probe++) {
        if (!index->hash_keys[slot]) return false;
        if (index->hash_keys[slot] == block) {
            *result = index->hash_values[slot];
            return true;
        }
        slot = (slot + 1) & (index->hash_cap - 1);
    }
    return false;
}

static bool mark_reachable_iterative(const block_index_t *index,
                                     anvil_block_t *entry,
                                     bool *reachable,
                                     anvil_block_t **worklist)
{
    if (!entry) return true;
    size_t entry_index;
    if (!block_index_find(index, entry, &entry_index)) {
        anvil_set_error(index->ctx, ANVIL_ERR_INVALID_ARG,
                        "Function entry is not in its block list");
        return false;
    }

    size_t head = 0;
    size_t tail = 0;
    reachable[entry_index] = true;
    worklist[tail++] = entry;

#define PUSH_REACHABLE(target_) do {                                      \
        anvil_block_t *target_value_ = (target_);                         \
        size_t target_index_;                                             \
        if (target_value_ &&                                              \
            block_index_find(index, target_value_, &target_index_) &&     \
            !reachable[target_index_]) {                                  \
            reachable[target_index_] = true;                              \
            worklist[tail++] = target_value_;                             \
        }                                                                 \
    } while (0)
    while (head < tail) {
        anvil_instr_t *term = worklist[head++]->last;
        if (!term) continue;
        if (term->op == ANVIL_OP_BR) {
            PUSH_REACHABLE(term->true_block);
        } else if (term->op == ANVIL_OP_BR_COND) {
            PUSH_REACHABLE(term->true_block);
            PUSH_REACHABLE(term->false_block);
        } else if (term->op == ANVIL_OP_SWITCH) {
            PUSH_REACHABLE(term->true_block);
            for (size_t i = 0; i < term->num_switch_cases; i++) {
                if (term->switch_blocks)
                    PUSH_REACHABLE(term->switch_blocks[i]);
            }
        }
    }
#undef PUSH_REACHABLE
    return true;
}

static bool switch_target_seen(const anvil_instr_t *term, size_t before,
                               anvil_block_t *target)
{
    if (!target) return true;
    if (term->true_block == target) return true;
    for (size_t i = 0; i < before; i++) {
        if (term->switch_blocks && term->switch_blocks[i] == target)
            return true;
    }
    return false;
}

static bool add_pred_count(const block_index_t *index, size_t *counts,
                           anvil_block_t *target)
{
    size_t target_index;
    if (!target || !block_index_find(index, target, &target_index)) return true;
    if (counts[target_index] == SIZE_MAX) {
        anvil_set_error(index->ctx, ANVIL_ERR_NOMEM,
                        "CFG predecessor count overflow");
        return false;
    }
    counts[target_index]++;
    return true;
}

/* Walk the function once and populate block->preds for every block. We
 * refresh this cache at the top of each do-while iteration; count_preds
 * below then runs in O(1) instead of O(n) per call. */
static bool recompute_preds(anvil_func_t *func, const block_index_t *index)
{
    anvil_ctx_t *ctx = func->parent->ctx;
    size_t *counts = anvil_ctx_calloc(ctx, index->count, sizeof(*counts));
    anvil_block_t ***new_preds = anvil_ctx_calloc(
        ctx, index->count, sizeof(*new_preds));
    size_t *written = anvil_ctx_calloc(ctx, index->count, sizeof(*written));
    if (!counts || !new_preds || !written) goto fail;

    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        if (term->op == ANVIL_OP_BR && term->true_block) {
            if (!add_pred_count(index, counts, term->true_block)) goto fail;
        } else if (term->op == ANVIL_OP_BR_COND) {
            if (!add_pred_count(index, counts, term->true_block)) goto fail;
            if (term->false_block != term->true_block &&
                !add_pred_count(index, counts, term->false_block)) goto fail;
        } else if (term->op == ANVIL_OP_SWITCH) {
            if (!add_pred_count(index, counts, term->true_block)) goto fail;
            for (size_t i = 0; i < term->num_switch_cases; i++) {
                anvil_block_t *target = term->switch_blocks
                    ? term->switch_blocks[i] : NULL;
                if (!switch_target_seen(term, i, target) &&
                    !add_pred_count(index, counts, target)) goto fail;
            }
        }
    }

    for (size_t i = 0; i < index->count; i++) {
        if (counts[i]) {
            new_preds[i] = anvil_ctx_malloc(
                ctx, counts[i] * sizeof(*new_preds[i]));
            if (!new_preds[i]) goto fail;
        }
    }

#define ADD_PRED(target_, pred_) do {                                      \
        size_t target_index_;                                              \
        if ((target_) && block_index_find(index, (target_), &target_index_)) \
            new_preds[target_index_][written[target_index_]++] = (pred_);  \
    } while (0)
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        anvil_instr_t *term = block->last;
        if (!term) continue;
        if (term->op == ANVIL_OP_BR) {
            ADD_PRED(term->true_block, block);
        } else if (term->op == ANVIL_OP_BR_COND) {
            ADD_PRED(term->true_block, block);
            if (term->false_block != term->true_block)
                ADD_PRED(term->false_block, block);
        } else if (term->op == ANVIL_OP_SWITCH) {
            ADD_PRED(term->true_block, block);
            for (size_t i = 0; i < term->num_switch_cases; i++) {
                anvil_block_t *target = term->switch_blocks
                    ? term->switch_blocks[i] : NULL;
                if (!switch_target_seen(term, i, target))
                    ADD_PRED(target, block);
            }
        }
    }
#undef ADD_PRED

    for (size_t i = 0; i < index->count; i++) {
        free(index->blocks[i]->preds);
        index->blocks[i]->preds = new_preds[i];
        index->blocks[i]->num_preds = counts[i];
        new_preds[i] = NULL;
    }
    free(counts);
    free(new_preds);
    free(written);
    return true;

fail:
    if (new_preds) {
        for (size_t i = 0; i < index->count; i++) free(new_preds[i]);
    }
    free(counts);
    free(new_preds);
    free(written);
    return false;
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
        } else if (term->op == ANVIL_OP_SWITCH) {
            if (term->true_block == old_block) {
                term->true_block = new_block;
            }
            for (size_t i = 0; i < term->num_switch_cases; i++) {
                if (term->switch_blocks && term->switch_blocks[i] == old_block) {
                    term->switch_blocks[i] = new_block;
                }
            }
        }
    }
}

/* Return true when pred already has an edge to target.  Redirecting another
 * edge from pred through an empty block to target would collapse two distinct
 * control-flow paths into the same predecessor.  A PHI can attach different
 * values to those paths before the redirect, but ANVIL's PHIs and backends key
 * incoming values by predecessor block, so that rewrite is not representable
 * without first materialising a select or splitting an edge. */
static bool block_targets(anvil_block_t *pred, anvil_block_t *target)
{
    anvil_instr_t *term = pred ? pred->last : NULL;
    if (!term || !target) return false;

    if (term->op == ANVIL_OP_BR) {
        return term->true_block == target;
    }
    if (term->op == ANVIL_OP_BR_COND) {
        return term->true_block == target || term->false_block == target;
    }
    if (term->op == ANVIL_OP_SWITCH) {
        if (term->true_block == target) return true;
        for (size_t i = 0; i < term->num_switch_cases; i++) {
            if (term->switch_blocks && term->switch_blocks[i] == target) {
                return true;
            }
        }
    }
    return false;
}

static bool copy_block_preds(anvil_block_t *block,
                             anvil_block_t ***out_preds,
                             size_t *out_count)
{
    *out_preds = NULL;
    *out_count = block ? block->num_preds : 0;
    if (!block || block->num_preds == 0) return true;

    anvil_ctx_t *ctx = block->parent && block->parent->parent
        ? block->parent->parent->ctx : NULL;
    anvil_block_t **preds = anvil_ctx_malloc(
        ctx, block->num_preds * sizeof(*preds));
    if (!preds) return false;

    memcpy(preds, block->preds, block->num_preds * sizeof(*preds));
    *out_preds = preds;
    return true;
}

static void remove_phi_incoming(anvil_instr_t *instr, size_t index)
{
    if (!instr || index >= instr->num_phi_incoming) return;

    for (size_t i = index + 1; i < instr->num_phi_incoming; i++) {
        instr->operands[i - 1] = instr->operands[i];
        instr->phi_blocks[i - 1] = instr->phi_blocks[i];
    }

    instr->num_operands--;
    instr->num_phi_incoming--;
}

typedef struct {
    anvil_instr_t *instr;
    anvil_value_t **operands;
    anvil_block_t **blocks;
    size_t count;
} phi_rewrite_plan_t;

static bool rewrite_phi_predecessor(anvil_block_t *block,
                                    anvil_block_t *old_pred,
                                    anvil_block_t **new_preds,
                                    size_t num_new_preds)
{
    if (!block) return true;

    /* Removing or renaming an incoming edge does not allocate and is therefore
     * atomic.  The expanding case below preflights every PHI before committing
     * any of them, so OOM cannot leave a partially rewritten CFG. */
    if (num_new_preds <= 1) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            if (instr->op != ANVIL_OP_PHI) break;
            for (size_t i = 0; i < instr->num_phi_incoming; ) {
                if (instr->phi_blocks[i] != old_pred) {
                    i++;
                } else if (num_new_preds == 0) {
                    remove_phi_incoming(instr, i);
                } else {
                    instr->phi_blocks[i++] = new_preds[0];
                }
            }
        }
        return true;
    }

    anvil_ctx_t *ctx = block->parent && block->parent->parent
        ? block->parent->parent->ctx : NULL;
    if (!ctx) return false;

    for (size_t p = 0; p < num_new_preds; p++) {
        if (!new_preds[p]) {
            anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                            "Cannot add a null PHI predecessor");
            return false;
        }
        for (size_t q = 0; q < p; q++) {
            if (new_preds[p] == new_preds[q]) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                "Cannot add duplicate PHI predecessors");
                return false;
            }
        }
    }

    size_t num_plans = 0;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (instr->op != ANVIL_OP_PHI) break;
        for (size_t i = 0; i < instr->num_phi_incoming; i++) {
            if (instr->phi_blocks[i] == old_pred) {
                num_plans++;
                break;
            }
        }
    }
    if (num_plans == 0) return true;

    phi_rewrite_plan_t *plans = anvil_ctx_calloc(
        ctx, num_plans, sizeof(*plans));
    if (!plans) return false;

    size_t plan_index = 0;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (instr->op != ANVIL_OP_PHI) break;
        size_t old_index = SIZE_MAX;
        for (size_t i = 0; i < instr->num_phi_incoming; i++) {
            if (instr->phi_blocks[i] == old_pred) {
                if (old_index != SIZE_MAX) {
                    anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                    "PHI contains duplicate predecessor");
                    goto fail;
                }
                old_index = i;
            } else {
                for (size_t p = 0; p < num_new_preds; p++) {
                    if (instr->phi_blocks[i] == new_preds[p]) {
                        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                        "PHI predecessor rewrite would collide");
                        goto fail;
                    }
                }
            }
        }
        if (old_index == SIZE_MAX) continue;

        if (instr->num_phi_incoming > SIZE_MAX - (num_new_preds - 1)) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "PHI incoming count overflow");
            goto fail;
        }
        size_t new_count = instr->num_phi_incoming + num_new_preds - 1;
        phi_rewrite_plan_t *plan = &plans[plan_index++];
        plan->instr = instr;
        plan->count = new_count;
        plan->operands = anvil_ctx_malloc(
            ctx, new_count * sizeof(*plan->operands));
        plan->blocks = anvil_ctx_malloc(
            ctx, new_count * sizeof(*plan->blocks));
        if (!plan->operands || !plan->blocks) goto fail;

        size_t out = 0;
        for (size_t i = 0; i < instr->num_phi_incoming; i++) {
            if (i != old_index) {
                plan->operands[out] = instr->operands[i];
                plan->blocks[out++] = instr->phi_blocks[i];
                continue;
            }
            for (size_t p = 0; p < num_new_preds; p++) {
                plan->operands[out] = instr->operands[i];
                plan->blocks[out++] = new_preds[p];
            }
        }
    }

    for (size_t i = 0; i < num_plans; i++) {
        anvil_instr_t *instr = plans[i].instr;
        free(instr->operands);
        free(instr->phi_blocks);
        instr->operands = plans[i].operands;
        instr->phi_blocks = plans[i].blocks;
        instr->num_operands = plans[i].count;
        instr->num_phi_incoming = plans[i].count;
        plans[i].operands = NULL;
        plans[i].blocks = NULL;
    }
    free(plans);
    return true;

fail:
    for (size_t i = 0; i < num_plans; i++) {
        free(plans[i].operands);
        free(plans[i].blocks);
    }
    free(plans);
    return false;
}

/* Rewrite (or remove) PHI incoming blocks in every successor named by term.
 * Repeated switch destinations are harmless: the first call rewrites all
 * matching incoming entries and subsequent calls find none. */
static bool rewrite_successor_phis(anvil_instr_t *term,
                                   anvil_block_t *old_pred,
                                   anvil_block_t *new_pred)
{
    anvil_block_t *replacement[1] = { new_pred };
    anvil_block_t **new_preds = new_pred ? replacement : NULL;
    size_t num_new_preds = new_pred ? 1 : 0;

    if (!term) return true;
    if (term->op == ANVIL_OP_BR) {
        return rewrite_phi_predecessor(term->true_block, old_pred,
                                       new_preds, num_new_preds);
    } else if (term->op == ANVIL_OP_BR_COND) {
        if (!rewrite_phi_predecessor(term->true_block, old_pred,
                                     new_preds, num_new_preds)) return false;
        if (term->false_block != term->true_block &&
            !rewrite_phi_predecessor(term->false_block, old_pred,
                                     new_preds, num_new_preds)) return false;
    } else if (term->op == ANVIL_OP_SWITCH) {
        if (!rewrite_phi_predecessor(term->true_block, old_pred,
                                     new_preds, num_new_preds)) return false;
        for (size_t i = 0; i < term->num_switch_cases; i++) {
            anvil_block_t *target = term->switch_blocks
                ? term->switch_blocks[i] : NULL;
            if (!switch_target_seen(term, i, target) &&
                !rewrite_phi_predecessor(target, old_pred,
                                         new_preds, num_new_preds)) {
                return false;
            }
        }
    }
    return true;
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
static bool simplify_const_branch(anvil_block_t *block)
{
    anvil_instr_t *term = block->last;
    if (!term || term->op != ANVIL_OP_BR_COND) return false;
    if (term->num_operands < 1) return false;
    
    anvil_value_t *cond = term->operands[0];
    if (cond->kind != ANVIL_VAL_CONST_INT) return false;
    
    int64_t val = cond->data.i;
    anvil_block_t *target = val ? term->true_block : term->false_block;
    anvil_block_t *dead_target = val ? term->false_block : term->true_block;

    if (dead_target && dead_target != target) {
        rewrite_phi_predecessor(dead_target, block, NULL, 0);
    }

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

    /* The moved terminator now leaves `block`, not the removed `succ`. */
    (void)rewrite_successor_phis(block->last, succ, block);
    
    /* Update branches to successor to point to this block */
    replace_branch_target(func, succ, block);
    
    /* Remove successor block */
    remove_block(func, succ);
    
    return true;
}

/* Simplify CFG pass */
anvil_pass_result_t anvil_pass_simplify_cfg(anvil_func_t *func)
{
    if (!func || !func->parent || !func->parent->ctx)
        return ANVIL_PASS_RUN_ERROR;
    anvil_ctx_t *ctx = func->parent->ctx;
    anvil_ctx_clear_error(ctx);
    if (!func->blocks) return ANVIL_PASS_RUN_UNCHANGED;
    
    bool changed = false;
    bool any_changed;
    
    do {
        any_changed = false;

        block_index_t index;
        if (!block_index_build(func, &index)) return ANVIL_PASS_RUN_ERROR;

        /* Refresh predecessor cache before any per-block queries. */
        if (!recompute_preds(func, &index)) {
            block_index_destroy(&index);
            return ANVIL_PASS_RUN_ERROR;
        }

        /* Simplify constant conditional branches */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            if (simplify_const_branch(block)) {
                any_changed = true;
                changed = true;
            }
        }

        if (any_changed) {
            block_index_destroy(&index);
            continue;
        }
        
        /* Remove empty blocks (blocks with just a branch) */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            /* Don't remove entry block */
            if (block == func->entry) continue;
            
            if (is_empty_block(block)) {
                anvil_block_t *target = block->last->true_block;
                if (target && target != block) {
                    anvil_block_t **old_preds = NULL;
                    size_t num_old_preds = 0;
                    if (!copy_block_preds(block, &old_preds, &num_old_preds)) {
                        block_index_destroy(&index);
                        return ANVIL_PASS_RUN_ERROR;
                    }

                    bool edge_collision = false;
                    for (size_t i = 0; i < num_old_preds; i++) {
                        if (block_targets(old_preds[i], target)) {
                            edge_collision = true;
                            break;
                        }
                    }
                    if (edge_collision) {
                        free(old_preds);
                        continue;
                    }

                    /* Allocate and validate every PHI replacement before
                     * redirecting any edge or unlinking the empty block. */
                    if (!rewrite_phi_predecessor(target, block, old_preds,
                                                 num_old_preds)) {
                        free(old_preds);
                        block_index_destroy(&index);
                        return ANVIL_PASS_RUN_ERROR;
                    }
                    replace_branch_target(func, block, target);
                    free(old_preds);
                    remove_block(func, block);
                    any_changed = true;
                    changed = true;
                    break; /* Pred caches and block iteration are now stale. */
                }
            }
        }

        if (any_changed) {
            block_index_destroy(&index);
            continue;
        }
        
        /* Try to merge blocks */
        for (anvil_block_t *block = func->blocks; block; block = block->next) {
            if (try_merge_blocks(func, block)) {
                any_changed = true;
                changed = true;
                break;  /* Restart iteration after modification */
            }
        }

        if (any_changed) {
            block_index_destroy(&index);
            continue;
        }
        
        /* Remove unreachable blocks */
        bool *reachable = anvil_ctx_calloc(ctx, index.count,
                                           sizeof(*reachable));
        anvil_block_t **worklist = anvil_ctx_malloc(
            ctx, index.count * sizeof(*worklist));
        if (!reachable || !worklist ||
            !mark_reachable_iterative(&index, func->entry,
                                      reachable, worklist)) {
            free(reachable);
            free(worklist);
            block_index_destroy(&index);
            return ANVIL_PASS_RUN_ERROR;
        }
            
        anvil_block_t *block = func->blocks;
        while (block) {
            anvil_block_t *next = block->next;
            size_t block_index;
            if (!block_index_find(&index, block, &block_index)) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                                "CFG block disappeared during reachability");
                free(reachable);
                free(worklist);
                block_index_destroy(&index);
                return ANVIL_PASS_RUN_ERROR;
            }
            if (!reachable[block_index] && block != func->entry) {
                (void)rewrite_successor_phis(block->last, block, NULL);
                remove_block(func, block);
                any_changed = true;
                changed = true;
            }
            block = next;
        }
        free(reachable);
        free(worklist);
        block_index_destroy(&index);
        
    } while (any_changed);
    
    if (anvil_ctx_get_last_error(ctx) != ANVIL_OK)
        return ANVIL_PASS_RUN_ERROR;
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}
