#include "regalloc.h"
#include "liveness.h"
#include <stdlib.h>
#include <string.h>

typedef struct LiveInterval {
    int vreg;
    int start;
    int end;
    int preg;
    int spill_slot;
} LiveInterval;

static int compare_intervals(const void* a, const void* b) {
    const LiveInterval* ia = (const LiveInterval*)a;
    const LiveInterval* ib = (const LiveInterval*)b;
    return ia->start - ib->start;
}

AnvilRegAllocResult* anvil_regalloc_linear_scan(AnvilMFunc* func, AnvilRegAllocConfig* config) {
    int max_vreg = func->next_vreg_id;
    
    LiveInterval* intervals = (LiveInterval*)calloc(max_vreg, sizeof(LiveInterval));
    for (int i = 0; i < max_vreg; i++) {
        intervals[i].vreg = i;
        intervals[i].start = -1;
        intervals[i].end = -1;
        intervals[i].preg = -1;
        intervals[i].spill_slot = -1;
    }
    
    int inst_num = 0;
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            for (int i = 0; i < inst->num_operands; i++) {
                if (inst->operands[i].kind == ANVIL_MOP_VREG) {
                    int vreg = inst->operands[i].vreg.id;
                    if (vreg >= 0 && vreg < max_vreg) {
                        if (intervals[vreg].start < 0) intervals[vreg].start = inst_num;
                        intervals[vreg].end = inst_num;
                    }
                }
            }
            for (int i = 0; i < inst->num_defs; i++) {
                if (inst->defs[i].kind == ANVIL_MOP_VREG) {
                    int vreg = inst->defs[i].vreg.id;
                    if (vreg >= 0 && vreg < max_vreg) {
                        if (intervals[vreg].start < 0) intervals[vreg].start = inst_num;
                        intervals[vreg].end = inst_num;
                    }
                }
            }
            inst_num++;
        }
    }
    
    int num_live = 0;
    for (int i = 0; i < max_vreg; i++) {
        if (intervals[i].start >= 0) num_live++;
    }
    
    LiveInterval* live_intervals = (LiveInterval*)malloc(sizeof(LiveInterval) * num_live);
    int li = 0;
    for (int i = 0; i < max_vreg; i++) {
        if (intervals[i].start >= 0) {
            live_intervals[li++] = intervals[i];
        }
    }
    
    qsort(live_intervals, num_live, sizeof(LiveInterval), compare_intervals);
    
    int* reg_end = (int*)malloc(sizeof(int) * config->num_available_regs);
    for (int i = 0; i < config->num_available_regs; i++) {
        reg_end[i] = -1;
    }
    
    int next_spill = 0;
    
    for (int p = 0; p < config->num_prealloc; p++) {
        int vreg = config->prealloc[p * 2];
        int preg = config->prealloc[p * 2 + 1];
        for (int i = 0; i < num_live; i++) {
            if (live_intervals[i].vreg == vreg) {
                live_intervals[i].preg = preg;
                for (int r = 0; r < config->num_available_regs; r++) {
                    if (config->available_regs[r] == preg) {
                        if (reg_end[r] < live_intervals[i].end) {
                            reg_end[r] = live_intervals[i].end;
                        }
                        break;
                    }
                }
                break;
            }
        }
    }
    
    for (int i = 0; i < num_live; i++) {
        LiveInterval* interval = &live_intervals[i];
        
        if (interval->preg >= 0) {
            continue;
        }
        
        for (int r = 0; r < config->num_available_regs; r++) {
            if (reg_end[r] >= 0 && reg_end[r] < interval->start) {
                reg_end[r] = -1;
            }
        }
        
        int best_reg = -1;
        for (int r = 0; r < config->num_available_regs; r++) {
            if (reg_end[r] < 0) {
                best_reg = r;
                break;
            }
        }
        
        if (best_reg >= 0) {
            interval->preg = config->available_regs[best_reg];
            reg_end[best_reg] = interval->end;
        } else {
            int furthest = -1;
            int furthest_reg = -1;
            for (int r = 0; r < config->num_available_regs; r++) {
                if (reg_end[r] > furthest) {
                    furthest = reg_end[r];
                    furthest_reg = r;
                }
            }
            
            if (furthest > interval->end) {
                for (int j = 0; j < i; j++) {
                    if (live_intervals[j].preg == config->available_regs[furthest_reg] &&
                        live_intervals[j].end == furthest) {
                        live_intervals[j].preg = -1;
                        live_intervals[j].spill_slot = next_spill++;
                        break;
                    }
                }
                interval->preg = config->available_regs[furthest_reg];
                reg_end[furthest_reg] = interval->end;
            } else {
                interval->spill_slot = next_spill++;
            }
        }
    }
    
    AnvilRegAllocResult* result = (AnvilRegAllocResult*)malloc(sizeof(AnvilRegAllocResult));
    result->num_vregs = max_vreg;
    result->vreg_to_preg = (int*)malloc(sizeof(int) * max_vreg);
    result->vreg_to_spill = (int*)malloc(sizeof(int) * max_vreg);
    result->num_spill_slots = next_spill;
    
    for (int i = 0; i < max_vreg; i++) {
        result->vreg_to_preg[i] = -1;
        result->vreg_to_spill[i] = -1;
    }
    
    for (int i = 0; i < num_live; i++) {
        int vreg = live_intervals[i].vreg;
        result->vreg_to_preg[vreg] = live_intervals[i].preg;
        result->vreg_to_spill[vreg] = live_intervals[i].spill_slot;
    }
    
    free(intervals);
    free(live_intervals);
    free(reg_end);
    
    return result;
}

static void replace_vreg_with_preg(AnvilMOperand* op, AnvilRegAllocResult* result) {
    if (op->kind == ANVIL_MOP_VREG) {
        int vreg = op->vreg.id;
        if (vreg >= 0 && vreg < result->num_vregs && result->vreg_to_preg[vreg] >= 0) {
            op->kind = ANVIL_MOP_PREG;
            op->preg.id = result->vreg_to_preg[vreg];
        }
    }
}

void anvil_regalloc_apply(AnvilMFunc* func, AnvilRegAllocResult* result, AnvilRegAllocConfig* config) {
    func->spill_slots = result->num_spill_slots;
    func->stack_size = result->num_spill_slots * config->stack_slot_size;
    
    for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
        AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
        for (AnvilMInst* inst = block->first; inst; inst = inst->next) {
            for (int i = 0; i < inst->num_operands; i++) {
                replace_vreg_with_preg(&inst->operands[i], result);
            }
            for (int i = 0; i < inst->num_defs; i++) {
                replace_vreg_with_preg(&inst->defs[i], result);
            }
        }
    }
}

void anvil_regalloc_result_free(AnvilRegAllocResult* result) {
    if (!result) return;
    free(result->vreg_to_preg);
    free(result->vreg_to_spill);
    free(result);
}
