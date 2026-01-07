#include "peephole.h"
#include "../regs.h"
#include <string.h>

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

bool ppc64_peephole_redundant_moves(AnvilMFunc* func) {
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

bool ppc64_peephole_copy_propagation(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_MOV && inst->num_operands >= 2 &&
                inst->operands[0].kind == ANVIL_MOP_PREG &&
                inst->operands[1].kind == ANVIL_MOP_PREG) {
                
                int tmp_reg = inst->operands[0].preg.id;
                int src_reg = inst->operands[1].preg.id;
                
                if (next->num_operands >= 2 &&
                    next->operands[0].kind == ANVIL_MOP_PREG &&
                    next->operands[0].preg.id == tmp_reg &&
                    next->next &&
                    next->next->kind == ANVIL_MIR_MOV &&
                    next->next->num_operands >= 2 &&
                    next->next->operands[1].kind == ANVIL_MOP_PREG &&
                    next->next->operands[1].preg.id == tmp_reg) {
                    
                    if (next->kind == ANVIL_MIR_ADD || next->kind == ANVIL_MIR_SUB ||
                        next->kind == ANVIL_MIR_MUL || next->kind == ANVIL_MIR_AND ||
                        next->kind == ANVIL_MIR_OR || next->kind == ANVIL_MIR_XOR) {
                        
                        int final_dst = next->next->operands[0].preg.id;
                        
                        next->operands[0].preg.id = final_dst;
                        
                        if (next->operands[1].kind == ANVIL_MOP_PREG &&
                            next->operands[1].preg.id == tmp_reg) {
                            next->operands[1].preg.id = src_reg;
                        }
                        
                        remove_inst(block, inst);
                        remove_inst(block, next->next);
                        changed = true;
                        inst = next;
                        continue;
                    }
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool ppc64_peephole_eliminate_move_chains(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; ) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_MOV && next->kind == ANVIL_MIR_MOV &&
                inst->num_operands >= 2 && next->num_operands >= 2) {
                
                if (inst->operands[0].kind == ANVIL_MOP_PREG &&
                    next->operands[1].kind == ANVIL_MOP_PREG &&
                    inst->operands[0].preg.id == next->operands[1].preg.id) {
                    
                    next->operands[1] = inst->operands[1];
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

bool ppc64_peephole_strength_reduce(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            if (inst->kind == ANVIL_MIR_MUL && inst->num_operands >= 2) {
                if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                    int64_t val = inst->operands[1].imm.value;
                    
                    if (val == 0) {
                        inst->kind = ANVIL_MIR_MOV;
                        inst->operands[1] = anvil_mop_imm(0, inst->operands[0].size);
                        changed = true;
                    } else if (val == 1) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                    } else if (val == 2) {
                        inst->kind = ANVIL_MIR_ADD;
                        inst->operands[1] = inst->operands[0];
                        changed = true;
                    } else if ((val & (val - 1)) == 0 && val > 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->kind = ANVIL_MIR_SHL;
                        inst->operands[1] = anvil_mop_imm(shift, inst->operands[0].size);
                        changed = true;
                    }
                }
            }
            
            if (inst->kind == ANVIL_MIR_DIV && inst->num_operands >= 2) {
                if (inst->operands[1].kind == ANVIL_MOP_IMM) {
                    int64_t val = inst->operands[1].imm.value;
                    
                    if (val == 1) {
                        inst->kind = ANVIL_MIR_NOP;
                        inst->num_operands = 0;
                        changed = true;
                    } else if ((val & (val - 1)) == 0 && val > 0) {
                        int shift = 0;
                        int64_t tmp = val;
                        while (tmp > 1) { tmp >>= 1; shift++; }
                        inst->kind = ANVIL_MIR_SAR;
                        inst->operands[1] = anvil_mop_imm(shift, inst->operands[0].size);
                        changed = true;
                    }
                }
            }
            
            if ((inst->kind == ANVIL_MIR_ADD || inst->kind == ANVIL_MIR_SUB) && 
                inst->num_operands >= 2) {
                if (inst->operands[1].kind == ANVIL_MOP_IMM && 
                    inst->operands[1].imm.value == 0) {
                    inst->kind = ANVIL_MIR_NOP;
                    inst->num_operands = 0;
                    changed = true;
                }
            }
        }
    }
    
    return changed;
}

bool ppc64_peephole_combine_instructions(AnvilMFunc* func) {
    bool changed = false;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        
        for (AnvilMInst* inst = block->first; inst && inst->next; inst = inst->next) {
            AnvilMInst* next = inst->next;
            
            if (inst->kind == ANVIL_MIR_CMP && next->kind == ANVIL_MIR_JCC) {
                if (inst->operands[1].kind == ANVIL_MOP_IMM && 
                    inst->operands[1].imm.value == 0) {
                }
            }
            
            if (inst->kind == ANVIL_MIR_MOV && next->kind == ANVIL_MIR_MOV) {
                if (inst->operands[0].kind == ANVIL_MOP_PREG &&
                    next->operands[1].kind == ANVIL_MOP_PREG &&
                    inst->operands[0].preg.id == next->operands[1].preg.id) {
                    if (inst->operands[1].kind == next->operands[0].kind) {
                    }
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
    const int max_iterations = 10;
    
    while (changed && iterations < max_iterations) {
        changed = false;
        iterations++;
        
        changed |= ppc64_peephole_copy_propagation(func);
        changed |= ppc64_peephole_eliminate_move_chains(func);
        changed |= ppc64_peephole_redundant_moves(func);
        changed |= ppc64_peephole_strength_reduce(func);
        changed |= ppc64_peephole_combine_instructions(func);
    }
}
