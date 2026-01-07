#include "isel.h"
#include <string.h>
#include <stdlib.h>

void anvil_isel_init(AnvilISelContext* ctx, AnvilMFunc* func, const AnvilISelRuleSet* ruleset) {
    memset(ctx, 0, sizeof(AnvilISelContext));
    ctx->func = func;
    ctx->arena = func->arena;
    ctx->ruleset = ruleset;
}

AnvilMInst* anvil_isel_create_inst(AnvilISelContext* ctx, AnvilMInstKind kind) {
    AnvilMInst* inst = (AnvilMInst*)anvil_arena_alloc(ctx->arena, sizeof(AnvilMInst));
    memset(inst, 0, sizeof(AnvilMInst));
    inst->kind = kind;
    inst->func = ctx->func;
    return inst;
}

bool anvil_isel_match_power_of_2(int64_t value, int* shift_out) {
    if (value <= 0) return false;
    if ((value & (value - 1)) != 0) return false;
    
    int shift = 0;
    int64_t tmp = value;
    while (tmp > 1) {
        tmp >>= 1;
        shift++;
    }
    
    if (shift_out) *shift_out = shift;
    return true;
}

bool anvil_isel_match_small_const(int64_t value, int bits) {
    int64_t max_val = (1LL << (bits - 1)) - 1;
    int64_t min_val = -(1LL << (bits - 1));
    return value >= min_val && value <= max_val;
}

static const AnvilISelRule* find_best_rule(AnvilISelContext* ctx, AnvilMInst* inst, AnvilISelMatch* match) {
    const AnvilISelRule* best = NULL;
    int best_cost = INT32_MAX;
    
    for (int i = 0; i < ctx->ruleset->num_rules; i++) {
        const AnvilISelRule* rule = &ctx->ruleset->rules[i];
        
        if (rule->src_kind != inst->kind) continue;
        
        AnvilISelMatch test_match = {0};
        test_match.inst = inst;
        
        if (rule->match && rule->match(ctx, inst, &test_match)) {
            int cost = rule->cost ? rule->cost(ctx, &test_match) : rule->priority;
            if (cost < best_cost) {
                best_cost = cost;
                best = rule;
                *match = test_match;
            }
        }
    }
    
    return best;
}

static void replace_inst_in_block(AnvilMBlock* block, AnvilMInst* old_inst, AnvilVec* new_insts) {
    if (anvil_vec_len(new_insts) == 0) {
        if (old_inst->prev) old_inst->prev->next = old_inst->next;
        else block->first = old_inst->next;
        if (old_inst->next) old_inst->next->prev = old_inst->prev;
        else block->last = old_inst->prev;
        block->inst_count--;
        return;
    }
    
    AnvilMInst* first_new = *(AnvilMInst**)anvil_vec_get(new_insts, 0);
    AnvilMInst* last_new = *(AnvilMInst**)anvil_vec_get(new_insts, anvil_vec_len(new_insts) - 1);
    
    for (size_t i = 0; i < anvil_vec_len(new_insts) - 1; i++) {
        AnvilMInst* curr = *(AnvilMInst**)anvil_vec_get(new_insts, i);
        AnvilMInst* next = *(AnvilMInst**)anvil_vec_get(new_insts, i + 1);
        curr->next = next;
        next->prev = curr;
    }
    
    first_new->prev = old_inst->prev;
    last_new->next = old_inst->next;
    
    if (old_inst->prev) old_inst->prev->next = first_new;
    else block->first = first_new;
    
    if (old_inst->next) old_inst->next->prev = last_new;
    else block->last = last_new;
    
    block->inst_count += (int)anvil_vec_len(new_insts) - 1;
}

void anvil_isel_run(AnvilISelContext* ctx) {
    if (!ctx->ruleset || ctx->ruleset->num_rules == 0) return;
    
    for (size_t bi = 0; bi < anvil_vec_len(&ctx->func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&ctx->func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            AnvilISelMatch match = {0};
            const AnvilISelRule* rule = find_best_rule(ctx, inst, &match);
            
            if (rule && rule->emit) {
                AnvilVec new_insts;
                anvil_vec_init(&new_insts, sizeof(AnvilMInst*));
                
                rule->emit(ctx, &match, &new_insts);
                
                if (anvil_vec_len(&new_insts) > 0) {
                    replace_inst_in_block(block, inst, &new_insts);
                }
                
                anvil_vec_free(&new_insts);
            }
            
            inst = next;
        }
    }
}
