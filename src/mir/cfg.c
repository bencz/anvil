#include "cfg.h"
#include "../core/str.h"
#include <string.h>

static void add_edge(AnvilMBlock* from, AnvilMBlock* to) {
    AnvilMBlock** succ = (AnvilMBlock**)anvil_vec_push(&from->succs);
    *succ = to;
    AnvilMBlock** pred = (AnvilMBlock**)anvil_vec_push(&to->preds);
    *pred = from;
}

void anvil_cfg_build(AnvilMFunc* func) {
    for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
        anvil_vec_clear(&block->preds);
        anvil_vec_clear(&block->succs);
    }
    
    for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
        if (!block->last) continue;
        
        AnvilMInst* term = block->last;
        
        if (term->kind == ANVIL_MIR_JMP) {
            const char* target_name = term->operands[0].label.name;
            for (size_t j = 0; j < anvil_vec_len(&func->blocks); j++) {
                AnvilMBlock* target = *(AnvilMBlock**)anvil_vec_get(&func->blocks, j);
                if (anvil_str_eq(target->name, target_name)) {
                    add_edge(block, target);
                    break;
                }
            }
        } else if (term->kind == ANVIL_MIR_JCC) {
            const char* target_name = term->operands[0].label.name;
            for (size_t j = 0; j < anvil_vec_len(&func->blocks); j++) {
                AnvilMBlock* target = *(AnvilMBlock**)anvil_vec_get(&func->blocks, j);
                if (anvil_str_eq(target->name, target_name)) {
                    add_edge(block, target);
                    break;
                }
            }
            if (i + 1 < anvil_vec_len(&func->blocks)) {
                AnvilMBlock* fallthrough = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i + 1);
                add_edge(block, fallthrough);
            }
        } else if (term->kind != ANVIL_MIR_RET) {
            if (i + 1 < anvil_vec_len(&func->blocks)) {
                AnvilMBlock* fallthrough = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i + 1);
                add_edge(block, fallthrough);
            }
        }
    }
}

static void dfs_post_order(AnvilMBlock* block, AnvilVec* visited, AnvilVec* order) {
    for (size_t i = 0; i < anvil_vec_len(visited); i++) {
        if (*(AnvilMBlock**)anvil_vec_get(visited, i) == block) return;
    }
    
    AnvilMBlock** v = (AnvilMBlock**)anvil_vec_push(visited);
    *v = block;
    
    for (size_t i = 0; i < anvil_vec_len(&block->succs); i++) {
        AnvilMBlock* succ = *(AnvilMBlock**)anvil_vec_get(&block->succs, i);
        dfs_post_order(succ, visited, order);
    }
    
    AnvilMBlock** o = (AnvilMBlock**)anvil_vec_push(order);
    *o = block;
}

void anvil_cfg_compute_post_order(AnvilMFunc* func, AnvilVec* order) {
    anvil_vec_init(order, sizeof(AnvilMBlock*));
    AnvilVec visited;
    anvil_vec_init(&visited, sizeof(AnvilMBlock*));
    
    if (func->entry) {
        dfs_post_order(func->entry, &visited, order);
    }
    
    anvil_vec_free(&visited);
}

void anvil_cfg_compute_reverse_post_order(AnvilMFunc* func, AnvilVec* order) {
    AnvilVec post_order;
    anvil_cfg_compute_post_order(func, &post_order);
    
    anvil_vec_init(order, sizeof(AnvilMBlock*));
    for (size_t i = anvil_vec_len(&post_order); i > 0; i--) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&post_order, i - 1);
        AnvilMBlock** slot = (AnvilMBlock**)anvil_vec_push(order);
        *slot = block;
    }
    
    anvil_vec_free(&post_order);
}

void anvil_cfg_compute_dominators(AnvilMFunc* func) {
    (void)func;
}
