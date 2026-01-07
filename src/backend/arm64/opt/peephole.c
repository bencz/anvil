#include "peephole.h"
#include "../regs.h"

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

bool arm64_peephole_remove_redundant_moves(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        AnvilMInst* inst = block->first;
        while (inst) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_MOV && inst->num_operands >= 2) {
                if (inst->operands[0].kind == ANVIL_MOP_PREG &&
                    inst->operands[1].kind == ANVIL_MOP_PREG &&
                    inst->operands[0].preg.id == inst->operands[1].preg.id) {
                    remove_inst(block, inst);
                    changed = true;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool arm64_peephole_fold_mov_op_mov(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next && inst->next->next; ) {
            AnvilMInst* op_inst = inst->next;
            AnvilMInst* mov2 = inst->next->next;
            
            if (inst->kind == ANVIL_MIR_MOV && inst->num_operands >= 2 &&
                mov2->kind == ANVIL_MIR_MOV && mov2->num_operands >= 2 &&
                inst->operands[0].kind == ANVIL_MOP_PREG &&
                inst->operands[1].kind == ANVIL_MOP_PREG &&
                mov2->operands[0].kind == ANVIL_MOP_PREG &&
                mov2->operands[1].kind == ANVIL_MOP_PREG) {
                
                bool is_binary_op = (op_inst->kind == ANVIL_MIR_ADD || 
                                     op_inst->kind == ANVIL_MIR_SUB ||
                                     op_inst->kind == ANVIL_MIR_MUL);
                
                if (is_binary_op && op_inst->num_operands >= 2 &&
                    op_inst->operands[0].kind == ANVIL_MOP_PREG) {
                    
                    int tmp_reg = inst->operands[0].preg.id;
                    int src_reg = inst->operands[1].preg.id;
                    int dst_reg = mov2->operands[0].preg.id;
                    
                    if (op_inst->operands[0].preg.id == tmp_reg &&
                        mov2->operands[1].preg.id == tmp_reg &&
                        src_reg == dst_reg) {
                        
                        op_inst->operands[0].preg.id = dst_reg;
                        
                        remove_inst(block, inst);
                        remove_inst(block, mov2);
                        changed = true;
                        inst = op_inst;
                        continue;
                    }
                }
            }
            
            inst = inst->next;
        }
    }
    
    return changed;
}

void arm64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    
    bool changed = true;
    int iterations = 0;
    const int max_iterations = 5;
    
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        changed |= arm64_peephole_fold_mov_op_mov(func);
        changed |= arm64_peephole_remove_redundant_moves(func);
    }
}
