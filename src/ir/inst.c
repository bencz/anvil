#include "inst.h"
#include "../core/str.h"

AnvilInst* anvil_inst_create(AnvilArena* arena, AnvilInstKind kind) {
    AnvilInst* inst = (AnvilInst*)anvil_arena_alloc(arena, sizeof(AnvilInst));
    inst->kind = kind;
    inst->result = NULL;
    inst->operands[0] = inst->operands[1] = inst->operands[2] = NULL;
    inst->num_operands = 0;
    inst->next = inst->prev = NULL;
    return inst;
}

AnvilInst* anvil_inst_binary(AnvilArena* arena, AnvilInstKind kind, AnvilValue* lhs, AnvilValue* rhs, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, kind);
    inst->operands[0] = lhs;
    inst->operands[1] = rhs;
    inst->num_operands = 2;
    inst->result = result;
    return inst;
}

AnvilInst* anvil_inst_unary(AnvilArena* arena, AnvilInstKind kind, AnvilValue* operand, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, kind);
    inst->operands[0] = operand;
    inst->num_operands = 1;
    inst->result = result;
    return inst;
}

AnvilInst* anvil_inst_ret(AnvilArena* arena, AnvilValue* val) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_RET);
    inst->operands[0] = val;
    inst->num_operands = val ? 1 : 0;
    return inst;
}

AnvilInst* anvil_inst_ret_void(AnvilArena* arena) {
    return anvil_inst_create(arena, ANVIL_INST_RET_VOID);
}

AnvilInst* anvil_inst_br(AnvilArena* arena, AnvilBlock* target) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_BR);
    inst->br.target = target;
    return inst;
}

AnvilInst* anvil_inst_br_cond(AnvilArena* arena, AnvilValue* cond, AnvilBlock* then_bb, AnvilBlock* else_bb) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_BR_COND);
    inst->operands[0] = cond;
    inst->num_operands = 1;
    inst->br_cond.then_block = then_bb;
    inst->br_cond.else_block = else_bb;
    return inst;
}

AnvilInst* anvil_inst_call(AnvilArena* arena, const char* func_name, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_CALL);
    inst->call.func_name = anvil_arena_strdup(arena, func_name);
    anvil_vec_init(&inst->call.args, sizeof(AnvilValue*));
    inst->result = result;
    return inst;
}

AnvilInst* anvil_inst_load(AnvilArena* arena, AnvilValue* ptr, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_LOAD);
    inst->operands[0] = ptr;
    inst->num_operands = 1;
    inst->result = result;
    return inst;
}

AnvilInst* anvil_inst_store(AnvilArena* arena, AnvilValue* ptr, AnvilValue* val) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_STORE);
    inst->operands[0] = ptr;
    inst->operands[1] = val;
    inst->num_operands = 2;
    return inst;
}

AnvilInst* anvil_inst_alloca(AnvilArena* arena, AnvilType* type, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, ANVIL_INST_ALLOCA);
    inst->cast.target_type = type;
    inst->result = result;
    return inst;
}

AnvilInst* anvil_inst_cast(AnvilArena* arena, AnvilInstKind kind, AnvilValue* val, AnvilType* target_type, AnvilValue* result) {
    AnvilInst* inst = anvil_inst_create(arena, kind);
    inst->operands[0] = val;
    inst->num_operands = 1;
    inst->cast.target_type = target_type;
    inst->result = result;
    return inst;
}

void anvil_inst_add_arg(AnvilInst* call_inst, AnvilValue* arg) {
    if (call_inst->kind != ANVIL_INST_CALL && call_inst->kind != ANVIL_INST_CALL_INDIRECT) return;
    AnvilValue** slot = (AnvilValue**)anvil_vec_push(&call_inst->call.args);
    *slot = arg;
}
