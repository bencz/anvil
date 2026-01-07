#include "peephole.h"
#include "../regs.h"

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

bool x86_64_peephole_remove_redundant_moves(AnvilMFunc* func) {
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

bool x86_64_peephole_eliminate_frame_for_leaf(AnvilMFunc* func) {
    (void)func;
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
        
        changed |= x86_64_peephole_remove_redundant_moves(func);
    }
}
