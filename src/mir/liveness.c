#include "liveness.h"
#include "cfg.h"
#include <stdlib.h>
#include <string.h>

static void collect_uses_defs(AnvilMInst* inst, AnvilVec* uses, AnvilVec* defs) {
    for (int i = 0; i < inst->num_operands; i++) {
        AnvilMOperand* op = &inst->operands[i];
        if (op->kind == ANVIL_MOP_VREG) {
            int* use = (int*)anvil_vec_push(uses);
            *use = op->vreg.id;
        }
        if (op->kind == ANVIL_MOP_MEM) {
            if (op->mem.base_is_vreg && op->mem.base_reg >= 0) {
                int* use = (int*)anvil_vec_push(uses);
                *use = op->mem.base_reg;
            }
            if (op->mem.index_is_vreg && op->mem.index_reg >= 0) {
                int* use = (int*)anvil_vec_push(uses);
                *use = op->mem.index_reg;
            }
        }
    }
    
    for (int i = 0; i < inst->num_defs; i++) {
        AnvilMOperand* op = &inst->defs[i];
        if (op->kind == ANVIL_MOP_VREG) {
            int* def = (int*)anvil_vec_push(defs);
            *def = op->vreg.id;
        }
    }
}

static bool set_contains(AnvilVec* set, int val) {
    for (size_t i = 0; i < anvil_vec_len(set); i++) {
        if (*(int*)anvil_vec_get(set, i) == val) return true;
    }
    return false;
}

static bool set_add(AnvilVec* set, int val) {
    if (set_contains(set, val)) return false;
    int* slot = (int*)anvil_vec_push(set);
    *slot = val;
    return true;
}

static void set_union(AnvilVec* dst, AnvilVec* src) {
    for (size_t i = 0; i < anvil_vec_len(src); i++) {
        set_add(dst, *(int*)anvil_vec_get(src, i));
    }
}

static void set_subtract(AnvilVec* dst, AnvilVec* src) {
    for (size_t i = 0; i < anvil_vec_len(dst); ) {
        int val = *(int*)anvil_vec_get(dst, i);
        if (set_contains(src, val)) {
            anvil_vec_remove(dst, i);
        } else {
            i++;
        }
    }
}

AnvilLivenessInfo* anvil_liveness_compute(AnvilMFunc* func) {
    anvil_cfg_build(func);
    
    AnvilLivenessInfo* info = (AnvilLivenessInfo*)malloc(sizeof(AnvilLivenessInfo));
    info->num_blocks = (int)anvil_vec_len(&func->blocks);
    info->live_in = (AnvilVec*)malloc(sizeof(AnvilVec) * info->num_blocks);
    info->live_out = (AnvilVec*)malloc(sizeof(AnvilVec) * info->num_blocks);
    
    for (int i = 0; i < info->num_blocks; i++) {
        anvil_vec_init(&info->live_in[i], sizeof(int));
        anvil_vec_init(&info->live_out[i], sizeof(int));
    }
    
    AnvilVec* use_sets = (AnvilVec*)malloc(sizeof(AnvilVec) * info->num_blocks);
    AnvilVec* def_sets = (AnvilVec*)malloc(sizeof(AnvilVec) * info->num_blocks);
    
    for (int i = 0; i < info->num_blocks; i++) {
        anvil_vec_init(&use_sets[i], sizeof(int));
        anvil_vec_init(&def_sets[i], sizeof(int));
        
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            AnvilVec inst_uses, inst_defs;
            anvil_vec_init(&inst_uses, sizeof(int));
            anvil_vec_init(&inst_defs, sizeof(int));
            
            collect_uses_defs(inst, &inst_uses, &inst_defs);
            
            for (size_t j = 0; j < anvil_vec_len(&inst_uses); j++) {
                int vreg = *(int*)anvil_vec_get(&inst_uses, j);
                if (!set_contains(&def_sets[i], vreg)) {
                    set_add(&use_sets[i], vreg);
                }
            }
            
            for (size_t j = 0; j < anvil_vec_len(&inst_defs); j++) {
                int vreg = *(int*)anvil_vec_get(&inst_defs, j);
                set_add(&def_sets[i], vreg);
            }
            
            anvil_vec_free(&inst_uses);
            anvil_vec_free(&inst_defs);
        }
    }
    
    bool changed = true;
    while (changed) {
        changed = false;
        
        for (int i = info->num_blocks - 1; i >= 0; i--) {
            AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
            
            AnvilVec new_out;
            anvil_vec_init(&new_out, sizeof(int));
            
            for (size_t j = 0; j < anvil_vec_len(&block->succs); j++) {
                AnvilMBlock* succ = *(AnvilMBlock**)anvil_vec_get(&block->succs, j);
                for (int k = 0; k < info->num_blocks; k++) {
                    AnvilMBlock* b = *(AnvilMBlock**)anvil_vec_get(&func->blocks, k);
                    if (b == succ) {
                        set_union(&new_out, &info->live_in[k]);
                        break;
                    }
                }
            }
            
            if (anvil_vec_len(&new_out) != anvil_vec_len(&info->live_out[i])) {
                changed = true;
            }
            anvil_vec_free(&info->live_out[i]);
            info->live_out[i] = new_out;
            
            AnvilVec new_in;
            anvil_vec_init(&new_in, sizeof(int));
            set_union(&new_in, &info->live_out[i]);
            set_subtract(&new_in, &def_sets[i]);
            set_union(&new_in, &use_sets[i]);
            
            if (anvil_vec_len(&new_in) != anvil_vec_len(&info->live_in[i])) {
                changed = true;
            }
            anvil_vec_free(&info->live_in[i]);
            info->live_in[i] = new_in;
        }
    }
    
    for (int i = 0; i < info->num_blocks; i++) {
        anvil_vec_free(&use_sets[i]);
        anvil_vec_free(&def_sets[i]);
    }
    free(use_sets);
    free(def_sets);
    
    return info;
}

void anvil_liveness_free(AnvilLivenessInfo* info) {
    if (!info) return;
    for (int i = 0; i < info->num_blocks; i++) {
        anvil_vec_free(&info->live_in[i]);
        anvil_vec_free(&info->live_out[i]);
    }
    free(info->live_in);
    free(info->live_out);
    free(info);
}

bool anvil_liveness_is_live_at(AnvilLivenessInfo* info, int vreg, AnvilMBlock* block) {
    (void)info;
    return set_contains(&block->live_in, vreg) || set_contains(&block->live_out, vreg);
}

void anvil_liveness_get_live_range(AnvilMFunc* func, int vreg, int* first_use, int* last_use) {
    *first_use = -1;
    *last_use = -1;
    int inst_num = 0;
    
    for (size_t i = 0; i < anvil_vec_len(&func->blocks); i++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, i);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            for (int j = 0; j < inst->num_operands; j++) {
                if (inst->operands[j].kind == ANVIL_MOP_VREG &&
                    inst->operands[j].vreg.id == vreg) {
                    if (*first_use < 0) *first_use = inst_num;
                    *last_use = inst_num;
                }
            }
            for (int j = 0; j < inst->num_defs; j++) {
                if (inst->defs[j].kind == ANVIL_MOP_VREG &&
                    inst->defs[j].vreg.id == vreg) {
                    if (*first_use < 0) *first_use = inst_num;
                    *last_use = inst_num;
                }
            }
            inst_num++;
        }
    }
}
