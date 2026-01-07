#ifndef ANVIL_FUNC_H
#define ANVIL_FUNC_H

#include "inst.h"
#include "value.h"
#include "types_internal.h"
#include "../core/arena.h"
#include "../core/vec.h"
#include "../core/hash.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilBlock {
    const char* name;
    int id;
    AnvilInst* first;
    AnvilInst* last;
    int inst_count;
    
    AnvilVec preds;
    AnvilVec succs;
    
    struct AnvilFunc* func;
    struct AnvilBlock* next;
    struct AnvilBlock* prev;
} AnvilBlock;

typedef struct AnvilFunc {
    const char* name;
    AnvilType* ret_type;
    AnvilType* func_type;
    
    AnvilVec params;
    AnvilVec locals;
    
    AnvilBlock* entry;
    AnvilBlock* current;
    AnvilVec blocks;
    int block_count;
    
    uint32_t next_temp_id;
    uint32_t next_block_id;
    
    AnvilArena* arena;
    struct AnvilModule* module;
    struct AnvilFunc* next;
    
    AnvilVec loop_stack;
    AnvilVec if_stack;
} AnvilFunc;

typedef struct AnvilLoop {
    AnvilBlock* header;
    AnvilBlock* body;
    AnvilBlock* exit;
    AnvilVar* var;
    AnvilValue* end_val;
    AnvilValue* step_val;
} AnvilLoop;

typedef struct AnvilIf {
    AnvilBlock* then_block;
    AnvilBlock* else_block;
    AnvilBlock* merge_block;
    bool in_else;
} AnvilIf;

AnvilBlock* anvil_block_create(AnvilArena* arena, AnvilFunc* func, const char* name);
void anvil_block_append_inst(AnvilBlock* block, AnvilInst* inst);
void anvil_block_prepend_inst(AnvilBlock* block, AnvilInst* inst);
bool anvil_block_is_terminated(const AnvilBlock* block);

AnvilFunc* anvil_func_create(AnvilArena* arena, const char* name, AnvilType* ret_type);
AnvilVar* anvil_func_add_param(AnvilFunc* func, const char* name, AnvilType* type);
AnvilVar* anvil_func_add_local(AnvilFunc* func, const char* name, AnvilType* type);
AnvilBlock* anvil_func_add_block(AnvilFunc* func, const char* name);
AnvilValue* anvil_func_new_temp(AnvilFunc* func, AnvilType* type);

void anvil_func_set_current_block(AnvilFunc* func, AnvilBlock* block);
AnvilBlock* anvil_func_get_current_block(AnvilFunc* func);
void anvil_func_emit(AnvilFunc* func, AnvilInst* inst);

#ifdef __cplusplus
}
#endif

#endif
