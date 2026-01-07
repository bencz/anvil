#include "peephole.h"
#include "../regs.h"

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

static bool x86_64_combine_add_to_lea(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_ADD && next->kind == ANVIL_MIR_ADD &&
                inst->num_operands >= 2 && next->num_operands >= 2 &&
                !inst->operands[0].is_fp && !next->operands[0].is_fp) {
                
                if (inst->operands[0].kind == ANVIL_MOP_PREG &&
                    next->operands[0].kind == ANVIL_MOP_PREG &&
                    inst->operands[0].preg.id == next->operands[0].preg.id &&
                    inst->operands[1].kind == ANVIL_MOP_PREG &&
                    next->operands[1].kind == ANVIL_MOP_IMM) {
                    
                    next->kind = ANVIL_MIR_LEA;
                    remove_inst(block, inst);
                    changed = true;
                    inst = next;
                    continue;
                }
            }
            inst = next;
        }
    }
    return changed;
}

static bool x86_64_combine_cmp_jcc_to_test(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_CMP && next->kind == ANVIL_MIR_JCC &&
                inst->num_operands >= 2) {
                
                if (inst->operands[1].kind == ANVIL_MOP_IMM &&
                    inst->operands[1].imm.value == 0 &&
                    (next->cc == ANVIL_CC_EQ || next->cc == ANVIL_CC_NE)) {
                    
                    inst->kind = ANVIL_MIR_TEST;
                    inst->operands[1] = inst->operands[0];
                    changed = true;
                }
            }
            inst = next;
        }
    }
    return changed;
}

static bool x86_64_eliminate_redundant_movs(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            bool is_mov = inst->kind == ANVIL_MIR_MOV || 
                          inst->kind == ANVIL_MIR_MOVSD || 
                          inst->kind == ANVIL_MIR_MOVSS;
            
            if (is_mov && inst->num_operands >= 2 &&
                inst->operands[0].kind == ANVIL_MOP_PREG &&
                inst->operands[1].kind == ANVIL_MOP_PREG &&
                inst->operands[0].preg.id == inst->operands[1].preg.id) {
                remove_inst(block, inst);
                changed = true;
            }
            inst = next;
        }
    }
    return changed;
}

static bool inst_reads_preg(AnvilMInst* inst, int preg_id) {
    for (int i = 0; i < inst->num_operands; i++) {
        if (inst->operands[i].kind == ANVIL_MOP_PREG) {
            bool is_dest = (i == 0 && (inst->kind == ANVIL_MIR_MOV || 
                                       inst->kind == ANVIL_MIR_MOVSD ||
                                       inst->kind == ANVIL_MIR_MOVSS));
            if (!is_dest && inst->operands[i].preg.id == preg_id) {
                return true;
            }
            if (i > 0 && inst->operands[i].preg.id == preg_id) {
                return true;
            }
        }
    }
    return false;
}

static bool inst_writes_preg(AnvilMInst* inst, int preg_id) {
    if (inst->num_operands >= 1 && inst->operands[0].kind == ANVIL_MOP_PREG) {
        bool is_write = (inst->kind == ANVIL_MIR_MOV || 
                        inst->kind == ANVIL_MIR_MOVSD ||
                        inst->kind == ANVIL_MIR_MOVSS);
        if (is_write && inst->operands[0].preg.id == preg_id) {
            return true;
        }
    }
    return false;
}

static bool x86_64_eliminate_dead_stores(AnvilMFunc* func) {
    bool changed = false;
    
    const int XMM0_ID = 16;
    const int RAX_ID = 0;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next_inst = inst->next;
            
            bool is_mov = (inst->kind == ANVIL_MIR_MOV || inst->kind == ANVIL_MIR_MOVSD || 
                          inst->kind == ANVIL_MIR_MOVSS) && inst->num_operands >= 2;
            
            if (is_mov && inst->operands[0].kind == ANVIL_MOP_PREG) {
                int dest_preg = inst->operands[0].preg.id;
                
                bool is_return_reg = (dest_preg == XMM0_ID || dest_preg == RAX_ID);
                
                bool is_read = false;
                bool is_overwritten = false;
                for (AnvilMInst* scan = inst->next; scan; scan = scan->next) {
                    if (scan->kind == ANVIL_MIR_RET) {
                        break;
                    }
                    if (inst_reads_preg(scan, dest_preg)) {
                        is_read = true;
                        break;
                    }
                    if (inst_writes_preg(scan, dest_preg)) {
                        is_overwritten = true;
                        break;
                    }
                }
                
                if (is_overwritten || (!is_read && !is_return_reg)) {
                    remove_inst(block, inst);
                    changed = true;
                    inst = next_inst;
                    continue;
                }
            }
            
            inst = next_inst;
        }
    }
    return changed;
}

static bool x86_64_propagate_copy_to_op(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next) {
            AnvilMInst* mov = inst;
            AnvilMInst* op = inst->next;
            
            bool is_mov = (mov->kind == ANVIL_MIR_MOV || mov->kind == ANVIL_MIR_MOVSD || 
                          mov->kind == ANVIL_MIR_MOVSS) && mov->num_operands >= 2;
            bool is_fp_op = (op->kind == ANVIL_MIR_FADD || op->kind == ANVIL_MIR_FSUB ||
                            op->kind == ANVIL_MIR_FMUL || op->kind == ANVIL_MIR_FDIV) && 
                           op->num_operands >= 2;
            
            if (is_mov && is_fp_op) {
                AnvilMOperand* mov_dst = &mov->operands[0];
                AnvilMOperand* mov_src = &mov->operands[1];
                
                if (mov_dst->kind == ANVIL_MOP_PREG && mov_src->kind == ANVIL_MOP_PREG &&
                    op->operands[1].kind == ANVIL_MOP_PREG &&
                    op->operands[1].preg.id == mov_dst->preg.id) {
                    
                    if (op->operands[0].kind == ANVIL_MOP_PREG &&
                        op->operands[0].preg.id == mov_src->preg.id) {
                        inst = inst->next;
                        continue;
                    }
                    
                    bool tmp_used_later = false;
                    for (AnvilMInst* scan = op->next; scan; scan = scan->next) {
                        if (inst_reads_preg(scan, mov_dst->preg.id)) {
                            tmp_used_later = true;
                            break;
                        }
                        if (inst_writes_preg(scan, mov_dst->preg.id)) {
                            break;
                        }
                    }
                    
                    if (!tmp_used_later) {
                        op->operands[1] = *mov_src;
                        remove_inst(block, mov);
                        changed = true;
                        inst = op;
                        continue;
                    }
                }
            }
            
            inst = inst->next;
        }
    }
    return changed;
}

static bool x86_64_coalesce_mov_op_mov(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next && inst->next->next) {
            AnvilMInst* mov1 = inst;
            AnvilMInst* op = inst->next;
            AnvilMInst* mov2 = inst->next->next;
            
            bool is_mov1 = (mov1->kind == ANVIL_MIR_MOV || mov1->kind == ANVIL_MIR_MOVSD || 
                           mov1->kind == ANVIL_MIR_MOVSS) && mov1->num_operands >= 2;
            bool is_fp_op = (op->kind == ANVIL_MIR_FADD || op->kind == ANVIL_MIR_FSUB ||
                            op->kind == ANVIL_MIR_FMUL || op->kind == ANVIL_MIR_FDIV) && 
                           op->num_operands >= 2;
            bool is_mov2 = (mov2->kind == ANVIL_MIR_MOV || mov2->kind == ANVIL_MIR_MOVSD ||
                           mov2->kind == ANVIL_MIR_MOVSS) && mov2->num_operands >= 2;
            
            if (is_mov1 && is_fp_op && is_mov2) {
                AnvilMOperand* tmp = &mov1->operands[0];
                AnvilMOperand* src = &mov1->operands[1];
                AnvilMOperand* op_dst = &op->operands[0];
                AnvilMOperand* final_dst = &mov2->operands[0];
                AnvilMOperand* mov2_src = &mov2->operands[1];
                
                if (tmp->kind == ANVIL_MOP_PREG && op_dst->kind == ANVIL_MOP_PREG &&
                    mov2_src->kind == ANVIL_MOP_PREG &&
                    tmp->preg.id == op_dst->preg.id && tmp->preg.id == mov2_src->preg.id) {
                    
                    if (src->kind == ANVIL_MOP_PREG && final_dst->kind == ANVIL_MOP_PREG &&
                        src->preg.id == final_dst->preg.id) {
                        
                        op->operands[0] = *final_dst;
                        
                        remove_inst(block, mov1);
                        remove_inst(block, mov2);
                        
                        changed = true;
                        inst = op;
                        continue;
                    }
                }
            }
            
            inst = inst->next;
        }
    }
    return changed;
}

static bool x86_64_use_inc_dec(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if ((inst->kind == ANVIL_MIR_ADD || inst->kind == ANVIL_MIR_SUB) &&
                inst->num_operands >= 2 &&
                inst->operands[1].kind == ANVIL_MOP_IMM) {
                
                int64_t val = inst->operands[1].imm.value;
                if (val == 1 || val == -1) {
                    changed = true;
                }
            }
        }
    }
    return changed;
}

bool x86_64_peephole_eliminate_frame_for_leaf(AnvilMFunc* func) {
    bool has_call = false;
    bool uses_stack = func->stack_size > 0;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_CALL || 
                inst->kind == ANVIL_MIR_CALL_INDIRECT ||
                inst->kind == ANVIL_MIR_CALL_PLT) {
                has_call = true;
                break;
            }
        }
        if (has_call) break;
    }
    
    if (!has_call && !uses_stack) {
        func->is_leaf = true;
        return true;
    }
    return false;
}

void x86_64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    
    bool changed = true;
    int iterations = 0;
    const int max_iterations = 5;
    
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        changed |= x86_64_eliminate_dead_stores(func);
        changed |= x86_64_propagate_copy_to_op(func);
        changed |= x86_64_coalesce_mov_op_mov(func);
        changed |= x86_64_eliminate_redundant_movs(func);
        changed |= x86_64_combine_add_to_lea(func);
        changed |= x86_64_combine_cmp_jcc_to_test(func);
        changed |= x86_64_use_inc_dec(func);
        changed |= x86_64_peephole_eliminate_frame_for_leaf(func);
    }
}
