#include "func.h"
#include "../core/str.h"
#include <string.h>

AnvilBlock* anvil_block_create(AnvilArena* arena, AnvilFunc* func, const char* name) {
    AnvilBlock* block = (AnvilBlock*)anvil_arena_alloc(arena, sizeof(AnvilBlock));
    block->name = anvil_arena_strdup(arena, name);
    block->id = func ? func->next_block_id++ : 0;
    block->first = NULL;
    block->last = NULL;
    block->inst_count = 0;
    anvil_vec_init(&block->preds, sizeof(AnvilBlock*));
    anvil_vec_init(&block->succs, sizeof(AnvilBlock*));
    block->func = func;
    block->next = NULL;
    block->prev = NULL;
    return block;
}

void anvil_block_append_inst(AnvilBlock* block, AnvilInst* inst) {
    if (!block || !inst) return;
    inst->next = NULL;
    inst->prev = block->last;
    if (block->last) {
        block->last->next = inst;
    } else {
        block->first = inst;
    }
    block->last = inst;
    block->inst_count++;
}

void anvil_block_prepend_inst(AnvilBlock* block, AnvilInst* inst) {
    if (!block || !inst) return;
    inst->prev = NULL;
    inst->next = block->first;
    if (block->first) {
        block->first->prev = inst;
    } else {
        block->last = inst;
    }
    block->first = inst;
    block->inst_count++;
}

bool anvil_block_is_terminated(const AnvilBlock* block) {
    if (!block || !block->last) return false;
    switch (block->last->kind) {
        case ANVIL_INST_RET:
        case ANVIL_INST_RET_VOID:
        case ANVIL_INST_BR:
        case ANVIL_INST_BR_COND:
            return true;
        default:
            return false;
    }
}

AnvilFunc* anvil_func_create(AnvilArena* arena, const char* name, AnvilType* ret_type) {
    AnvilFunc* func = (AnvilFunc*)anvil_arena_alloc(arena, sizeof(AnvilFunc));
    func->name = anvil_arena_strdup(arena, name);
    func->ret_type = ret_type;
    func->func_type = NULL;
    anvil_vec_init(&func->params, sizeof(AnvilVar*));
    anvil_vec_init(&func->locals, sizeof(AnvilVar*));
    anvil_vec_init(&func->blocks, sizeof(AnvilBlock*));
    func->block_count = 0;
    func->next_temp_id = 1;
    func->next_block_id = 0;
    func->arena = arena;
    func->module = NULL;
    func->next = NULL;
    anvil_vec_init(&func->loop_stack, sizeof(AnvilLoop));
    anvil_vec_init(&func->if_stack, sizeof(AnvilIf));
    
    func->entry = anvil_block_create(arena, func, "entry");
    AnvilBlock** slot = (AnvilBlock**)anvil_vec_push(&func->blocks);
    *slot = func->entry;
    func->block_count++;
    func->current = func->entry;
    
    return func;
}

AnvilVar* anvil_func_add_param(AnvilFunc* func, const char* name, AnvilType* type) {
    int index = (int)anvil_vec_len(&func->params);
    AnvilVar* var = anvil_var_create(func->arena, name, type, index, true);
    anvil_value_from_var(func->arena, var);
    AnvilVar** slot = (AnvilVar**)anvil_vec_push(&func->params);
    *slot = var;
    return var;
}

AnvilVar* anvil_func_add_local(AnvilFunc* func, const char* name, AnvilType* type) {
    int index = (int)anvil_vec_len(&func->locals);
    AnvilVar* var = anvil_var_create(func->arena, name, type, index, false);
    anvil_value_from_var(func->arena, var);
    AnvilVar** slot = (AnvilVar**)anvil_vec_push(&func->locals);
    *slot = var;
    return var;
}

AnvilBlock* anvil_func_add_block(AnvilFunc* func, const char* name) {
    AnvilBlock* block = anvil_block_create(func->arena, func, name);
    AnvilBlock** slot = (AnvilBlock**)anvil_vec_push(&func->blocks);
    *slot = block;
    func->block_count++;
    
    if (func->entry) {
        AnvilBlock* last = func->entry;
        while (last->next) last = last->next;
        last->next = block;
        block->prev = last;
    }
    return block;
}

AnvilValue* anvil_func_new_temp(AnvilFunc* func, AnvilType* type) {
    return anvil_value_temp(func->arena, type, func->next_temp_id++);
}

void anvil_func_set_current_block(AnvilFunc* func, AnvilBlock* block) {
    func->current = block;
}

AnvilBlock* anvil_func_get_current_block(AnvilFunc* func) {
    return func->current;
}

void anvil_func_emit(AnvilFunc* func, AnvilInst* inst) {
    if (func->current) {
        anvil_block_append_inst(func->current, inst);
    }
}
