#include "mir.h"
#include "../core/str.h"
#include <string.h>

AnvilMIR* anvil_mir_create(AnvilArena* arena) {
    AnvilMIR* mir = (AnvilMIR*)anvil_arena_alloc(arena, sizeof(AnvilMIR));
    mir->arena = arena;
    mir->first_func = NULL;
    mir->last_func = NULL;
    mir->func_count = 0;
    mir->strings = NULL;
    mir->string_count = 0;
    mir->string_capacity = 0;
    return mir;
}

void anvil_mir_add_string(AnvilMIR* mir, int id, const char* value) {
    if (mir->string_count >= mir->string_capacity) {
        int new_cap = mir->string_capacity == 0 ? 8 : mir->string_capacity * 2;
        AnvilMString* new_strings = (AnvilMString*)anvil_arena_alloc(
            mir->arena, sizeof(AnvilMString) * new_cap);
        if (mir->strings) {
            for (int i = 0; i < mir->string_count; i++) {
                new_strings[i] = mir->strings[i];
            }
        }
        mir->strings = new_strings;
        mir->string_capacity = new_cap;
    }
    mir->strings[mir->string_count].id = id;
    mir->strings[mir->string_count].value = anvil_arena_strdup(mir->arena, value);
    mir->string_count++;
}

AnvilMFunc* anvil_mir_add_func(AnvilMIR* mir, const char* name, AnvilType* ret_type) {
    AnvilMFunc* func = anvil_mfunc_create(mir->arena, name, ret_type);
    if (mir->last_func) {
        mir->last_func->next = func;
        mir->last_func = func;
    } else {
        mir->first_func = mir->last_func = func;
    }
    mir->func_count++;
    return func;
}

AnvilMFunc* anvil_mfunc_create(AnvilArena* arena, const char* name, AnvilType* ret_type) {
    AnvilMFunc* func = (AnvilMFunc*)anvil_arena_alloc(arena, sizeof(AnvilMFunc));
    func->name = anvil_arena_strdup(arena, name);
    func->ret_type = ret_type;
    anvil_vec_init(&func->params, sizeof(AnvilMOperand));
    func->num_params = 0;
    func->entry = NULL;
    anvil_vec_init(&func->blocks, sizeof(AnvilMBlock*));
    func->block_count = 0;
    func->next_vreg_id = 1;
    func->next_block_id = 0;
    func->stack_size = 0;
    func->spill_slots = 0;
    func->arena = arena;
    func->next = NULL;
    
    func->entry = anvil_mfunc_add_block(func, "entry");
    return func;
}

AnvilMBlock* anvil_mfunc_add_block(AnvilMFunc* func, const char* name) {
    AnvilMBlock* block = anvil_mblock_create(func->arena, func, name);
    AnvilMBlock** slot = (AnvilMBlock**)anvil_vec_push(&func->blocks);
    *slot = block;
    func->block_count++;
    
    if (func->entry && func->entry != block) {
        AnvilMBlock* last = func->entry;
        while (last->next) last = last->next;
        last->next = block;
        block->prev = last;
    }
    return block;
}

int anvil_mfunc_new_vreg(AnvilMFunc* func) {
    return func->next_vreg_id++;
}

AnvilMBlock* anvil_mblock_create(AnvilArena* arena, AnvilMFunc* func, const char* name) {
    AnvilMBlock* block = (AnvilMBlock*)anvil_arena_alloc(arena, sizeof(AnvilMBlock));
    block->name = anvil_arena_strdup(arena, name);
    block->id = func ? func->next_block_id++ : 0;
    block->first = NULL;
    block->last = NULL;
    block->inst_count = 0;
    anvil_vec_init(&block->preds, sizeof(AnvilMBlock*));
    anvil_vec_init(&block->succs, sizeof(AnvilMBlock*));
    anvil_vec_init(&block->live_in, sizeof(int));
    anvil_vec_init(&block->live_out, sizeof(int));
    block->func = func;
    block->next = NULL;
    block->prev = NULL;
    return block;
}

void anvil_mblock_append(AnvilMBlock* block, AnvilMInst* inst) {
    if (!block || !inst) return;
    inst->next = NULL;
    inst->prev = block->last;
    inst->func = block->func;
    if (block->last) {
        block->last->next = inst;
    } else {
        block->first = inst;
    }
    block->last = inst;
    block->inst_count++;
}

void anvil_mblock_prepend(AnvilMBlock* block, AnvilMInst* inst) {
    if (!block || !inst) return;
    inst->prev = NULL;
    inst->next = block->first;
    inst->func = block->func;
    if (block->first) {
        block->first->prev = inst;
    } else {
        block->last = inst;
    }
    block->first = inst;
    block->inst_count++;
}

void anvil_mblock_insert_before(AnvilMBlock* block, AnvilMInst* before, AnvilMInst* inst) {
    if (!block || !inst) return;
    if (!before) {
        anvil_mblock_append(block, inst);
        return;
    }
    inst->next = before;
    inst->prev = before->prev;
    if (before->prev) {
        before->prev->next = inst;
    } else {
        block->first = inst;
    }
    before->prev = inst;
    block->inst_count++;
}

void anvil_mblock_insert_after(AnvilMBlock* block, AnvilMInst* after, AnvilMInst* inst) {
    if (!block || !inst) return;
    if (!after) {
        anvil_mblock_prepend(block, inst);
        return;
    }
    inst->prev = after;
    inst->next = after->next;
    if (after->next) {
        after->next->prev = inst;
    } else {
        block->last = inst;
    }
    after->next = inst;
    block->inst_count++;
}

void anvil_mblock_remove(AnvilMBlock* block, AnvilMInst* inst) {
    if (!block || !inst) return;
    if (inst->prev) {
        inst->prev->next = inst->next;
    } else {
        block->first = inst->next;
    }
    if (inst->next) {
        inst->next->prev = inst->prev;
    } else {
        block->last = inst->prev;
    }
    block->inst_count--;
}

AnvilMInst* anvil_minst_create(AnvilArena* arena, AnvilMInstKind kind) {
    AnvilMInst* inst = (AnvilMInst*)anvil_arena_alloc(arena, sizeof(AnvilMInst));
    inst->kind = kind;
    inst->cc = ANVIL_CC_EQ;
    inst->num_operands = 0;
    inst->operands_capacity = 4;
    inst->operands = (AnvilMOperand*)anvil_arena_alloc(arena, sizeof(AnvilMOperand) * 4);
    inst->num_defs = 0;
    inst->comment = NULL;
    inst->is_variadic = false;
    inst->num_fixed_args = 0;
    inst->func = NULL;
    inst->next = inst->prev = NULL;
    for (int i = 0; i < 4; i++) inst->operands[i] = anvil_mop_none();
    for (int i = 0; i < 2; i++) inst->defs[i] = anvil_mop_none();
    return inst;
}

AnvilMInst* anvil_minst_create_with_capacity(AnvilArena* arena, AnvilMInstKind kind, int capacity) {
    AnvilMInst* inst = (AnvilMInst*)anvil_arena_alloc(arena, sizeof(AnvilMInst));
    inst->kind = kind;
    inst->cc = ANVIL_CC_EQ;
    inst->num_operands = 0;
    inst->operands_capacity = capacity;
    inst->operands = (AnvilMOperand*)anvil_arena_alloc(arena, sizeof(AnvilMOperand) * capacity);
    inst->num_defs = 0;
    inst->comment = NULL;
    inst->is_variadic = false;
    inst->num_fixed_args = 0;
    inst->func = NULL;
    inst->next = inst->prev = NULL;
    for (int i = 0; i < capacity; i++) inst->operands[i] = anvil_mop_none();
    for (int i = 0; i < 2; i++) inst->defs[i] = anvil_mop_none();
    return inst;
}

AnvilMInst* anvil_minst_mov(AnvilArena* arena, AnvilMOperand dst, AnvilMOperand src) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_MOV);
    inst->operands[0] = dst;
    inst->operands[1] = src;
    inst->num_operands = 2;
    inst->defs[0] = dst;
    inst->num_defs = 1;
    return inst;
}

AnvilMInst* anvil_minst_mov_imm(AnvilArena* arena, AnvilMOperand dst, int64_t imm, int size) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_MOV_IMM);
    inst->operands[0] = dst;
    inst->operands[1] = anvil_mop_imm(imm, size);
    inst->num_operands = 2;
    inst->defs[0] = dst;
    inst->num_defs = 1;
    return inst;
}

AnvilMInst* anvil_minst_binary(AnvilArena* arena, AnvilMInstKind kind, AnvilMOperand dst, AnvilMOperand src) {
    AnvilMInst* inst = anvil_minst_create(arena, kind);
    inst->operands[0] = dst;
    inst->operands[1] = src;
    inst->num_operands = 2;
    inst->defs[0] = dst;
    inst->num_defs = 1;
    return inst;
}

AnvilMInst* anvil_minst_unary(AnvilArena* arena, AnvilMInstKind kind, AnvilMOperand op) {
    AnvilMInst* inst = anvil_minst_create(arena, kind);
    inst->operands[0] = op;
    inst->num_operands = 1;
    inst->defs[0] = op;
    inst->num_defs = 1;
    return inst;
}

AnvilMInst* anvil_minst_cmp(AnvilArena* arena, AnvilMOperand lhs, AnvilMOperand rhs) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_CMP);
    inst->operands[0] = lhs;
    inst->operands[1] = rhs;
    inst->num_operands = 2;
    return inst;
}

AnvilMInst* anvil_minst_jmp(AnvilArena* arena, const char* label) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_JMP);
    inst->operands[0] = anvil_mop_label(label, 0);
    inst->num_operands = 1;
    return inst;
}

AnvilMInst* anvil_minst_jcc(AnvilArena* arena, AnvilCondCode cc, const char* label) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_JCC);
    inst->cc = cc;
    inst->operands[0] = anvil_mop_label(label, 0);
    inst->num_operands = 1;
    return inst;
}

AnvilMInst* anvil_minst_ret(AnvilArena* arena) {
    return anvil_minst_create(arena, ANVIL_MIR_RET);
}

AnvilMInst* anvil_minst_call(AnvilArena* arena, const char* func_name) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_CALL);
    inst->operands[0] = anvil_mop_func(func_name);
    inst->num_operands = 1;
    return inst;
}

AnvilMInst* anvil_minst_label(AnvilArena* arena, const char* name) {
    AnvilMInst* inst = anvil_minst_create(arena, ANVIL_MIR_LABEL);
    inst->operands[0] = anvil_mop_label(name, 0);
    inst->num_operands = 1;
    return inst;
}

AnvilMOperand anvil_mop_none(void) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_NONE;
    return op;
}

AnvilMOperand anvil_mop_vreg(int id, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_VREG;
    op.size = size;
    op.is_fp = false;
    op.vreg.id = id;
    return op;
}

AnvilMOperand anvil_mop_vreg_fp(int id, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_VREG;
    op.size = size;
    op.is_fp = true;
    op.vreg.id = id;
    return op;
}

AnvilMOperand anvil_mop_preg(int id, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_PREG;
    op.size = size;
    op.is_fp = false;
    op.preg.id = id;
    return op;
}

AnvilMOperand anvil_mop_preg_fp(int id, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_PREG;
    op.size = size;
    op.is_fp = true;
    op.preg.id = id;
    return op;
}

AnvilMOperand anvil_mop_imm(int64_t value, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_IMM;
    op.size = size;
    op.imm.value = value;
    return op;
}

AnvilMOperand anvil_mop_fimm(double value, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_FIMM;
    op.size = size;
    op.fimm.value = value;
    return op;
}

AnvilMOperand anvil_mop_mem(int base, int64_t disp, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_MEM;
    op.size = size;
    op.mem.base_reg = base;
    op.mem.index_reg = -1;
    op.mem.scale = 1;
    op.mem.disp = disp;
    op.mem.base_is_vreg = false;
    op.mem.index_is_vreg = false;
    return op;
}

AnvilMOperand anvil_mop_mem_indexed(int base, int index, int scale, int64_t disp, int size) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_MEM;
    op.size = size;
    op.mem.base_reg = base;
    op.mem.index_reg = index;
    op.mem.scale = scale;
    op.mem.disp = disp;
    op.mem.base_is_vreg = false;
    op.mem.index_is_vreg = false;
    return op;
}

AnvilMOperand anvil_mop_label(const char* name, int id) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_LABEL;
    op.label.name = name;
    op.label.id = id;
    return op;
}

AnvilMOperand anvil_mop_func(const char* name) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_FUNC;
    op.func.name = name;
    return op;
}

AnvilMOperand anvil_mop_global(const char* name) {
    AnvilMOperand op = {0};
    op.kind = ANVIL_MOP_GLOBAL;
    op.global.name = name;
    return op;
}
