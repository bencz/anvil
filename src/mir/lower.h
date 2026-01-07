#ifndef ANVIL_LOWER_H
#define ANVIL_LOWER_H

#include "mir.h"
#include "../ir/module.h"
#include "../ir/func.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AnvilABI;

typedef struct AnvilLowerCtx {
    AnvilArena* arena;
    AnvilModule* ir_module;
    AnvilMIR* mir;
    AnvilMFunc* current_func;
    AnvilMBlock* current_block;
    
    AnvilHash value_map;
    AnvilHash block_map;
    
    int next_vreg;
    int next_vreg_fp;
    
    const struct AnvilABI* abi;
    int ret_reg_int;
    int ret_reg_fp;
} AnvilLowerCtx;

AnvilMIR* anvil_lower_module(AnvilModule* mod);
AnvilMIR* anvil_lower_module_with_abi(AnvilModule* mod, const struct AnvilABI* abi);
void anvil_lower_func(AnvilLowerCtx* ctx, AnvilFunc* func);
void anvil_lower_block(AnvilLowerCtx* ctx, AnvilBlock* block);
void anvil_lower_inst(AnvilLowerCtx* ctx, AnvilInst* inst);

int anvil_lower_value(AnvilLowerCtx* ctx, AnvilValue* val);
AnvilMOperand anvil_lower_to_operand(AnvilLowerCtx* ctx, AnvilValue* val);

#ifdef __cplusplus
}
#endif

#endif
