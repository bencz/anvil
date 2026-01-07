#include "mir_opt.h"
#include <string.h>

static bool operands_equal(AnvilMOperand* a, AnvilMOperand* b) {
    if (a->kind != b->kind) return false;
    if (a->is_fp != b->is_fp) return false;
    
    switch (a->kind) {
        case ANVIL_MOP_PREG:
            return a->preg.id == b->preg.id;
        case ANVIL_MOP_VREG:
            return a->vreg.id == b->vreg.id && a->is_fp == b->is_fp;
        case ANVIL_MOP_IMM:
            return a->imm.value == b->imm.value;
        default:
            return false;
    }
}

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

bool anvil_mir_remove_redundant_moves(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_MOV && inst->num_operands >= 2) {
                if (operands_equal(&inst->operands[0], &inst->operands[1])) {
                    remove_inst(block, inst);
                    changed = true;
                    if (stats) stats->redundant_moves_removed++;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool anvil_mir_strength_reduce(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_MUL && inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_IMM) {
                    int64_t val = op->imm.value;
                    
                    int op_size = op->size;
                    
                    if (val == 0) {
                        inst->kind = ANVIL_MIR_MOV;
                        inst->operands[1] = anvil_mop_imm(0, op_size);
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                    else if (val == 1) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                    else if (val == 2) {
                        inst->kind = ANVIL_MIR_ADD;
                        inst->operands[1] = inst->operands[0];
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                    else if ((val & (val - 1)) == 0 && val > 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->kind = ANVIL_MIR_SHL;
                        inst->operands[1] = anvil_mop_imm(shift, op_size);
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                }
            }
            
            if (inst->kind == ANVIL_MIR_DIV && inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_IMM) {
                    int64_t val = op->imm.value;
                    
                    int op_size = op->size;
                    
                    if (val == 1) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                    else if ((val & (val - 1)) == 0 && val > 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->kind = ANVIL_MIR_SHR;
                        inst->operands[1] = anvil_mop_imm(shift, op_size);
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                }
            }
            
            if ((inst->kind == ANVIL_MIR_ADD || inst->kind == ANVIL_MIR_SUB) && 
                inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_IMM && op->imm.value == 0) {
                    inst->kind = ANVIL_MIR_NOP;
                    inst->num_operands = 0;
                    changed = true;
                    if (stats) stats->strength_reductions++;
                }
            }
            
            if (inst->kind == ANVIL_MIR_FMUL && inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_FIMM) {
                    double val = op->fimm.value;
                    if (val == 1.0) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    } else if (val == 2.0) {
                        inst->kind = ANVIL_MIR_FADD;
                        inst->operands[1] = inst->operands[0];
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    } else if (val == -1.0) {
                        inst->kind = ANVIL_MIR_FNEG;
                        inst->num_operands = 1;
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                }
            }
            
            if (inst->kind == ANVIL_MIR_FDIV && inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_FIMM) {
                    double val = op->fimm.value;
                    if (val == 1.0) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                        if (stats) stats->strength_reductions++;
                    }
                }
            }
            
            if ((inst->kind == ANVIL_MIR_FADD || inst->kind == ANVIL_MIR_FSUB) && 
                inst->num_operands >= 2) {
                AnvilMOperand* op = &inst->operands[1];
                if (op->kind == ANVIL_MOP_FIMM && op->fimm.value == 0.0) {
                    inst->kind = ANVIL_MIR_NOP;
                    inst->num_operands = 0;
                    changed = true;
                    if (stats) stats->strength_reductions++;
                }
            }
        }
    }
    
    return changed;
}

static bool is_vreg(AnvilMOperand* op) {
    return op->kind == ANVIL_MOP_VREG;
}

static bool is_preg(AnvilMOperand* op) {
    return op->kind == ANVIL_MOP_PREG;
}

bool anvil_mir_copy_propagation(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind != ANVIL_MIR_MOV || inst->num_operands < 2) continue;
            
            AnvilMOperand* dst = &inst->operands[0];
            AnvilMOperand* src = &inst->operands[1];
            
            if (!is_vreg(dst)) continue;
            if (!is_vreg(src) && !is_preg(src) && src->kind != ANVIL_MOP_IMM) continue;
            
            int dst_vreg = dst->vreg.id;
            
            for (AnvilMInst* use = inst->next; use; use = use->next) {
                if (use->kind == ANVIL_MIR_MOV && use->num_operands >= 2) {
                    if (is_vreg(&use->operands[1]) && use->operands[1].vreg.id == dst_vreg &&
                        use->operands[1].is_fp == dst->is_fp) {
                        if ((is_vreg(&use->operands[0]) || is_preg(&use->operands[0])) &&
                            use->operands[0].is_fp == src->is_fp) {
                            use->operands[1] = *src;
                            changed = true;
                            if (stats) stats->copy_propagations++;
                        }
                    }
                }
                
                for (int i = 0; i < use->num_defs; i++) {
                    if (is_vreg(&use->defs[i]) && use->defs[i].vreg.id == dst_vreg) {
                        goto next_inst;
                    }
                }
            }
            next_inst:;
        }
    }
    
    return changed;
}

bool anvil_mir_dead_code_elimination(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_NOP) {
                remove_inst(block, inst);
                changed = true;
                if (stats) stats->dead_code_removed++;
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool anvil_mir_eliminate_move_chains(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_MOV && next->kind == ANVIL_MIR_MOV &&
                inst->num_operands >= 2 && next->num_operands >= 2 &&
                inst->operands[0].is_fp == inst->operands[1].is_fp &&
                next->operands[0].is_fp == next->operands[1].is_fp) {
                
                if (operands_equal(&inst->operands[0], &next->operands[1])) {
                    next->operands[1] = inst->operands[1];
                    remove_inst(block, inst);
                    changed = true;
                    if (stats) stats->instructions_combined++;
                    inst = next;
                    continue;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

static bool is_mov_kind(AnvilMInstKind kind) {
    return kind == ANVIL_MIR_MOV || kind == ANVIL_MIR_MOVSD || kind == ANVIL_MIR_MOVSS;
}

static bool is_fp_binary_op(AnvilMInstKind kind) {
    return kind == ANVIL_MIR_FADD || kind == ANVIL_MIR_FSUB ||
           kind == ANVIL_MIR_FMUL || kind == ANVIL_MIR_FDIV;
}

bool anvil_mir_eliminate_dead_fp_moves(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next) {
            AnvilMInst* next = inst->next;
            
            if (is_mov_kind(inst->kind) && is_mov_kind(next->kind) &&
                inst->num_operands >= 2 && next->num_operands >= 2) {
                
                if (operands_equal(&inst->operands[0], &next->operands[0])) {
                    remove_inst(block, inst);
                    changed = true;
                    if (stats) stats->redundant_moves_removed++;
                    inst = next;
                    continue;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool anvil_mir_fold_mov_fp_binary(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next) {
            AnvilMInst* next = inst->next;
            
            if (is_mov_kind(inst->kind) && inst->num_operands >= 2 &&
                is_fp_binary_op(next->kind) && next->num_operands >= 2) {
                
                if (operands_equal(&inst->operands[0], &next->operands[0])) {
                    next->operands[0] = inst->operands[1];
                    remove_inst(block, inst);
                    changed = true;
                    if (stats) stats->instructions_combined++;
                    inst = next;
                    continue;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool anvil_mir_fold_mov_op_mov(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next && inst->next->next; ) {
            AnvilMInst* op_inst = inst->next;
            AnvilMInst* mov2 = inst->next->next;
            
            if (inst->kind == ANVIL_MIR_MOV && inst->num_operands >= 2 &&
                mov2->kind == ANVIL_MIR_MOV && mov2->num_operands >= 2) {
                
                bool is_binary_op = (op_inst->kind == ANVIL_MIR_ADD || 
                                     op_inst->kind == ANVIL_MIR_SUB ||
                                     op_inst->kind == ANVIL_MIR_MUL || 
                                     op_inst->kind == ANVIL_MIR_AND ||
                                     op_inst->kind == ANVIL_MIR_OR || 
                                     op_inst->kind == ANVIL_MIR_XOR ||
                                     is_fp_binary_op(op_inst->kind));
                
                if (is_binary_op && op_inst->num_operands >= 2) {
                    if (operands_equal(&inst->operands[0], &op_inst->operands[0]) &&
                        operands_equal(&op_inst->operands[0], &mov2->operands[1])) {
                        
                        AnvilMOperand final_dst = mov2->operands[0];
                        AnvilMOperand first_src = inst->operands[1];
                        
                        if (operands_equal(&first_src, &final_dst)) {
                            op_inst->operands[0] = final_dst;
                            remove_inst(block, inst);
                            remove_inst(block, mov2);
                            changed = true;
                            if (stats) stats->instructions_combined += 2;
                            inst = op_inst;
                            continue;
                        }
                    }
                }
            }
            
            inst = inst->next;
        }
    }
    
    return changed;
}

bool anvil_mir_peephole(AnvilMFunc* func, AnvilMirOptStats* stats) {
    bool changed = false;
    
    changed |= anvil_mir_eliminate_dead_fp_moves(func, stats);
    changed |= anvil_mir_fold_mov_op_mov(func, stats);
    changed |= anvil_mir_eliminate_move_chains(func, stats);
    changed |= anvil_mir_remove_redundant_moves(func, stats);
    changed |= anvil_mir_strength_reduce(func, stats);
    changed |= anvil_mir_copy_propagation(func, stats);
    changed |= anvil_mir_dead_code_elimination(func, stats);
    
    return changed;
}

void anvil_mir_analyze_function(AnvilMFunc* func) {
    func->is_leaf = true;
    func->needs_frame = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_CALL) {
                func->is_leaf = false;
            }
        }
    }
    
    if (!func->is_leaf || func->stack_size > 0 || func->spill_slots > 0) {
        func->needs_frame = true;
    }
}

void anvil_mir_opt_run_all(AnvilMIR* mir, int opt_level, AnvilMirOptStats* stats) {
    AnvilMirOptStats local_stats = {0};
    if (!stats) stats = &local_stats;
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        anvil_mir_analyze_function(func);
        
        if (opt_level <= 0) continue;
        
        bool changed = true;
        int iterations = 0;
        const int max_iterations = 5;
        
        while (changed && iterations < max_iterations) {
            changed = false;
            iterations++;
            
            if (opt_level >= 1) {
                changed |= anvil_mir_peephole(func, stats);
            }
        }
    }
}
