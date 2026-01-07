#include "peephole.h"
#include "../regs.h"

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

static bool ppc64_eliminate_redundant_movs(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            bool is_mov = inst->kind == ANVIL_MIR_MOV || 
                          inst->kind == ANVIL_MIR_MOVSD;
            
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
                                       inst->kind == ANVIL_MIR_MOVSD));
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
        bool is_write = (inst->kind == ANVIL_MIR_MOV || inst->kind == ANVIL_MIR_MOVSD);
        if (is_write && inst->operands[0].preg.id == preg_id) {
            return true;
        }
    }
    return false;
}

static bool ppc64_eliminate_dead_stores(AnvilMFunc* func) {
    bool changed = false;
    
    const int F1_ID = 33;
    const int R3_ID = 3;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next_inst = inst->next;
            
            bool is_mov = (inst->kind == ANVIL_MIR_MOV || inst->kind == ANVIL_MIR_MOVSD) && 
                          inst->num_operands >= 2;
            
            if (is_mov && inst->operands[0].kind == ANVIL_MOP_PREG) {
                int dest_preg = inst->operands[0].preg.id;
                
                bool is_return_reg = (dest_preg == F1_ID || dest_preg == R3_ID);
                
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

static bool ppc64_propagate_copy_to_op(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next) {
            AnvilMInst* mov = inst;
            AnvilMInst* op = inst->next;
            
            bool is_mov = (mov->kind == ANVIL_MIR_MOV || mov->kind == ANVIL_MIR_MOVSD) && 
                          mov->num_operands >= 2;
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

static bool ppc64_coalesce_mov_op_mov(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst && inst->next && inst->next->next) {
            AnvilMInst* mov1 = inst;
            AnvilMInst* op = inst->next;
            AnvilMInst* mov2 = inst->next->next;
            
            bool is_mov1 = (mov1->kind == ANVIL_MIR_MOV || mov1->kind == ANVIL_MIR_MOVSD) && 
                           mov1->num_operands >= 2;
            bool is_fp_op = (op->kind == ANVIL_MIR_FADD || op->kind == ANVIL_MIR_FSUB ||
                            op->kind == ANVIL_MIR_FMUL || op->kind == ANVIL_MIR_FDIV) && 
                           op->num_operands >= 2;
            bool is_mov2 = (mov2->kind == ANVIL_MIR_MOV || mov2->kind == ANVIL_MIR_MOVSD) && 
                           mov2->num_operands >= 2;
            
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

static bool ppc64_combine_load_update(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_LOAD && next->kind == ANVIL_MIR_ADD &&
                inst->num_operands >= 2 && next->num_operands >= 2) {
                
                if (inst->operands[1].kind == ANVIL_MOP_MEM &&
                    next->operands[0].kind == ANVIL_MOP_PREG &&
                    next->operands[1].kind == ANVIL_MOP_IMM) {
                    
                    int base_reg = inst->operands[1].mem.base_reg;
                    if (next->operands[0].preg.id == base_reg) {
                        inst->kind = ANVIL_MIR_LOAD_UPDATE;
                        remove_inst(block, next);
                        changed = true;
                        continue;
                    }
                }
            }
            inst = next;
        }
    }
    return changed;
}

static bool ppc64_use_addi_for_small_const(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_ADD && inst->num_operands >= 2 &&
                inst->operands[1].kind == ANVIL_MOP_IMM) {
                
                int64_t val = inst->operands[1].imm.value;
                if (val >= -32768 && val <= 32767) {
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void ppc64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    
    bool changed = true;
    int iterations = 0;
    const int max_iterations = 5;
    
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        changed |= ppc64_eliminate_dead_stores(func);
        changed |= ppc64_propagate_copy_to_op(func);
        changed |= ppc64_coalesce_mov_op_mov(func);
        changed |= ppc64_eliminate_redundant_movs(func);
        changed |= ppc64_combine_load_update(func);
        changed |= ppc64_use_addi_for_small_const(func);
    }
}
