#include "ir_opt.h"
#include "../ir/inst.h"
#include "../ir/value.h"
#include <string.h>

static bool is_const_int(AnvilValue* v) {
    return v && v->kind == ANVIL_VALUE_CONST_INT;
}

static int64_t get_const_int(AnvilValue* v) {
    return v->i64;
}

bool anvil_opt_const_fold(AnvilFunc* func, AnvilOptStats* stats) {
    bool changed = false;
    
    for (AnvilBlock* block = func->entry; block; block = block->next) {
        for (AnvilInst* inst = block->first; inst; inst = inst->next) {
            if (inst->num_operands < 2) continue;
            
            AnvilValue* lhs = inst->operands[0];
            AnvilValue* rhs = inst->operands[1];
            
            if (!is_const_int(lhs) || !is_const_int(rhs)) continue;
            
            int64_t a = get_const_int(lhs);
            int64_t b = get_const_int(rhs);
            int64_t result = 0;
            bool can_fold = true;
            
            switch (inst->kind) {
                case ANVIL_INST_ADD: result = a + b; break;
                case ANVIL_INST_SUB: result = a - b; break;
                case ANVIL_INST_MUL: result = a * b; break;
                case ANVIL_INST_DIV: 
                    if (b == 0) { can_fold = false; }
                    else { result = a / b; }
                    break;
                case ANVIL_INST_MOD:
                    if (b == 0) { can_fold = false; }
                    else { result = a % b; }
                    break;
                case ANVIL_INST_AND: result = a & b; break;
                case ANVIL_INST_OR:  result = a | b; break;
                case ANVIL_INST_XOR: result = a ^ b; break;
                case ANVIL_INST_SHL: result = a << b; break;
                case ANVIL_INST_SHR: result = (uint64_t)a >> b; break;
                case ANVIL_INST_SAR: result = a >> b; break;
                case ANVIL_INST_EQ:  result = (a == b) ? 1 : 0; break;
                case ANVIL_INST_NE:  result = (a != b) ? 1 : 0; break;
                case ANVIL_INST_LT:  result = (a < b) ? 1 : 0; break;
                case ANVIL_INST_LE:  result = (a <= b) ? 1 : 0; break;
                case ANVIL_INST_GT:  result = (a > b) ? 1 : 0; break;
                case ANVIL_INST_GE:  result = (a >= b) ? 1 : 0; break;
                default: can_fold = false; break;
            }
            
            if (can_fold && inst->result) {
                inst->result->kind = ANVIL_VALUE_CONST_INT;
                inst->result->i64 = result;
                inst->kind = ANVIL_INST_NOP;
                inst->num_operands = 0;
                changed = true;
                if (stats) stats->constants_folded++;
            }
        }
    }
    
    return changed;
}

static bool is_inst_used(AnvilFunc* func, AnvilValue* val) {
    if (!val) return false;
    
    for (AnvilBlock* block = func->entry; block; block = block->next) {
        for (AnvilInst* inst = block->first; inst; inst = inst->next) {
            for (int i = 0; i < inst->num_operands; i++) {
                if (inst->operands[i] == val) return true;
            }
        }
    }
    return false;
}

bool anvil_opt_dce(AnvilFunc* func, AnvilOptStats* stats) {
    bool changed = false;
    
    for (AnvilBlock* block = func->entry; block; block = block->next) {
        AnvilInst* inst = block->first;
        while (inst) {
            AnvilInst* next = inst->next;
            
            if (inst->kind == ANVIL_INST_NOP) {
                if (inst->prev) inst->prev->next = inst->next;
                else block->first = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else block->last = inst->prev;
                block->inst_count--;
                changed = true;
                if (stats) stats->dead_code_eliminated++;
            }
            else if (inst->result && !is_inst_used(func, inst->result)) {
                bool has_side_effects = false;
                switch (inst->kind) {
                    case ANVIL_INST_STORE:
                    case ANVIL_INST_CALL:
                    case ANVIL_INST_RET:
                    case ANVIL_INST_RET_VOID:
                    case ANVIL_INST_BR:
                    case ANVIL_INST_BR_COND:
                        has_side_effects = true;
                        break;
                    default:
                        break;
                }
                
                if (!has_side_effects) {
                    if (inst->prev) inst->prev->next = inst->next;
                    else block->first = inst->next;
                    if (inst->next) inst->next->prev = inst->prev;
                    else block->last = inst->prev;
                    block->inst_count--;
                    changed = true;
                    if (stats) stats->dead_code_eliminated++;
                }
            }
            
            inst = next;
        }
    }
    
    return changed;
}

bool anvil_opt_simplify_cfg(AnvilFunc* func, AnvilOptStats* stats) {
    bool changed = false;
    (void)func;
    (void)stats;
    return changed;
}

bool anvil_opt_mem2reg(AnvilFunc* func, AnvilOptStats* stats) {
    bool changed = false;
    (void)func;
    (void)stats;
    return changed;
}

void anvil_opt_run_all(AnvilModule* mod, int opt_level, AnvilOptStats* stats) {
    if (opt_level <= 0) return;
    
    AnvilOptStats local_stats = {0};
    if (!stats) stats = &local_stats;
    
    for (AnvilFunc* func = mod->first_func; func; func = func->next) {
        bool changed = true;
        int iterations = 0;
        const int max_iterations = 10;
        
        while (changed && iterations < max_iterations) {
            changed = false;
            iterations++;
            
            if (opt_level >= 1) {
                changed |= anvil_opt_const_fold(func, stats);
            }
            
            if (opt_level >= 1) {
                changed |= anvil_opt_dce(func, stats);
            }
            
            if (opt_level >= 2) {
                changed |= anvil_opt_simplify_cfg(func, stats);
            }
            
            if (opt_level >= 3) {
                changed |= anvil_opt_mem2reg(func, stats);
            }
        }
    }
}
