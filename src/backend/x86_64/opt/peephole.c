#include "peephole.h"
#include "../regs.h"

static void remove_inst(AnvilMBlock* block, AnvilMInst* inst) {
    if (inst->prev) inst->prev->next = inst->next;
    else block->first = inst->next;
    if (inst->next) inst->next->prev = inst->prev;
    else block->last = inst->prev;
    block->inst_count--;
}

bool x86_64_peephole_eliminate_frame_for_leaf(AnvilMFunc* func) {
    (void)func;
    return false;
}

void x86_64_peephole_run_all(AnvilBackend* backend, AnvilMFunc* func) {
    (void)backend;
    (void)func;
}
