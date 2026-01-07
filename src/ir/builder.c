#include "builder.h"
#include "../core/str.h"

static AnvilValue* build_binary(AnvilFunc* fn, AnvilInstKind kind, AnvilValue* lhs, AnvilValue* rhs) {
    AnvilValue* result = anvil_func_new_temp(fn, lhs->type);
    AnvilInst* inst = anvil_inst_binary(fn->arena, kind, lhs, rhs, result);
    anvil_func_emit(fn, inst);
    return result;
}

static AnvilValue* build_unary(AnvilFunc* fn, AnvilInstKind kind, AnvilValue* val) {
    AnvilValue* result = anvil_func_new_temp(fn, val->type);
    AnvilInst* inst = anvil_inst_unary(fn->arena, kind, val, result);
    anvil_func_emit(fn, inst);
    return result;
}

static AnvilValue* build_cmp(AnvilFunc* fn, AnvilInstKind kind, AnvilValue* lhs, AnvilValue* rhs) {
    AnvilValue* result = anvil_func_new_temp(fn, anvil_type_bool());
    AnvilInst* inst = anvil_inst_binary(fn->arena, kind, lhs, rhs, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_ADD, lhs, rhs);
}

AnvilValue* anvil_build_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_SUB, lhs, rhs);
}

AnvilValue* anvil_build_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_MUL, lhs, rhs);
}

AnvilValue* anvil_build_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_DIV, lhs, rhs);
}

AnvilValue* anvil_build_mod(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_MOD, lhs, rhs);
}

AnvilValue* anvil_build_neg(AnvilFunc* fn, AnvilValue* val) {
    return build_unary(fn, ANVIL_INST_NEG, val);
}

AnvilValue* anvil_build_and(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_AND, lhs, rhs);
}

AnvilValue* anvil_build_or(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_OR, lhs, rhs);
}

AnvilValue* anvil_build_xor(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_binary(fn, ANVIL_INST_XOR, lhs, rhs);
}

AnvilValue* anvil_build_not(AnvilFunc* fn, AnvilValue* val) {
    return build_unary(fn, ANVIL_INST_NOT, val);
}

AnvilValue* anvil_build_shl(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return build_binary(fn, ANVIL_INST_SHL, val, amount);
}

AnvilValue* anvil_build_shr(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return build_binary(fn, ANVIL_INST_SHR, val, amount);
}

AnvilValue* anvil_build_sar(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return build_binary(fn, ANVIL_INST_SAR, val, amount);
}

AnvilValue* anvil_build_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_EQ, lhs, rhs);
}

AnvilValue* anvil_build_ne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_NE, lhs, rhs);
}

AnvilValue* anvil_build_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_LT, lhs, rhs);
}

AnvilValue* anvil_build_le(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_LE, lhs, rhs);
}

AnvilValue* anvil_build_gt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_GT, lhs, rhs);
}

AnvilValue* anvil_build_ge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return build_cmp(fn, ANVIL_INST_GE, lhs, rhs);
}

AnvilValue* anvil_build_load(AnvilFunc* fn, AnvilVar* var) {
    AnvilValue* result = anvil_func_new_temp(fn, var->type);
    AnvilInst* inst = anvil_inst_load(fn->arena, var->value, result);
    anvil_func_emit(fn, inst);
    return result;
}

void anvil_build_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val) {
    AnvilInst* inst = anvil_inst_store(fn->arena, var->value, val);
    anvil_func_emit(fn, inst);
}

AnvilValue* anvil_build_addr_of(AnvilFunc* fn, AnvilVar* var) {
    AnvilType* ptr_type = anvil_type_ptr_create(fn->arena, var->type);
    AnvilValue* result = anvil_func_new_temp(fn, ptr_type);
    AnvilInst* inst = anvil_inst_unary(fn->arena, ANVIL_INST_ADDR_OF, var->value, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_deref(AnvilFunc* fn, AnvilValue* ptr) {
    AnvilType* pointee = ptr->type->ptr.pointee;
    AnvilValue* result = anvil_func_new_temp(fn, pointee);
    AnvilInst* inst = anvil_inst_unary(fn->arena, ANVIL_INST_DEREF, ptr, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_index(AnvilFunc* fn, AnvilValue* ptr, AnvilValue* idx) {
    AnvilValue* result = anvil_func_new_temp(fn, ptr->type);
    AnvilInst* inst = anvil_inst_binary(fn->arena, ANVIL_INST_INDEX, ptr, idx, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_field(AnvilFunc* fn, AnvilValue* struct_ptr, const char* field) {
    AnvilType* struct_type = struct_ptr->type->ptr.pointee;
    AnvilType* field_type = NULL;
    int field_index = -1;
    
    for (size_t i = 0; i < anvil_vec_len(&struct_type->struct_type.fields); i++) {
        AnvilStructField* f = (AnvilStructField*)anvil_vec_get(&struct_type->struct_type.fields, i);
        if (anvil_str_eq(f->name, field)) {
            field_type = f->type;
            field_index = (int)i;
            break;
        }
    }
    
    if (!field_type) return NULL;
    
    AnvilType* ptr_type = anvil_type_ptr_create(fn->arena, field_type);
    AnvilValue* result = anvil_func_new_temp(fn, ptr_type);
    AnvilInst* inst = anvil_inst_unary(fn->arena, ANVIL_INST_FIELD, struct_ptr, result);
    inst->field.field_name = anvil_arena_strdup(fn->arena, field);
    inst->field.field_index = field_index;
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_cast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    AnvilValue* result = anvil_func_new_temp(fn, to_type);
    AnvilInst* inst = anvil_inst_cast(fn->arena, ANVIL_INST_CAST, val, to_type, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_bitcast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    AnvilValue* result = anvil_func_new_temp(fn, to_type);
    AnvilInst* inst = anvil_inst_cast(fn->arena, ANVIL_INST_BITCAST, val, to_type, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_trunc(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    AnvilValue* result = anvil_func_new_temp(fn, to_type);
    AnvilInst* inst = anvil_inst_cast(fn->arena, ANVIL_INST_TRUNC, val, to_type, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_zext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    AnvilValue* result = anvil_func_new_temp(fn, to_type);
    AnvilInst* inst = anvil_inst_cast(fn->arena, ANVIL_INST_ZEXT, val, to_type, result);
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_sext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    AnvilValue* result = anvil_func_new_temp(fn, to_type);
    AnvilInst* inst = anvil_inst_cast(fn->arena, ANVIL_INST_SEXT, val, to_type, result);
    anvil_func_emit(fn, inst);
    return result;
}

void anvil_build_ret(AnvilFunc* fn, AnvilValue* val) {
    AnvilInst* inst = anvil_inst_ret(fn->arena, val);
    anvil_func_emit(fn, inst);
}

void anvil_build_ret_void(AnvilFunc* fn) {
    AnvilInst* inst = anvil_inst_ret_void(fn->arena);
    anvil_func_emit(fn, inst);
}

void anvil_build_br(AnvilFunc* fn, AnvilBlock* target) {
    AnvilInst* inst = anvil_inst_br(fn->arena, target);
    anvil_func_emit(fn, inst);
    
    AnvilBlock** succ = (AnvilBlock**)anvil_vec_push(&fn->current->succs);
    *succ = target;
    AnvilBlock** pred = (AnvilBlock**)anvil_vec_push(&target->preds);
    *pred = fn->current;
}

void anvil_build_br_cond(AnvilFunc* fn, AnvilValue* cond, AnvilBlock* then_bb, AnvilBlock* else_bb) {
    AnvilInst* inst = anvil_inst_br_cond(fn->arena, cond, then_bb, else_bb);
    anvil_func_emit(fn, inst);
    
    AnvilBlock** succ1 = (AnvilBlock**)anvil_vec_push(&fn->current->succs);
    *succ1 = then_bb;
    AnvilBlock** succ2 = (AnvilBlock**)anvil_vec_push(&fn->current->succs);
    *succ2 = else_bb;
    
    AnvilBlock** pred1 = (AnvilBlock**)anvil_vec_push(&then_bb->preds);
    *pred1 = fn->current;
    AnvilBlock** pred2 = (AnvilBlock**)anvil_vec_push(&else_bb->preds);
    *pred2 = fn->current;
}

AnvilIf* anvil_build_if_begin(AnvilFunc* fn, AnvilValue* cond) {
    AnvilIf* if_stmt = (AnvilIf*)anvil_vec_push(&fn->if_stack);
    
    if_stmt->then_block = anvil_func_add_block(fn, "if.then");
    if_stmt->else_block = anvil_func_add_block(fn, "if.else");
    if_stmt->merge_block = anvil_func_add_block(fn, "if.end");
    if_stmt->in_else = false;
    
    anvil_build_br_cond(fn, cond, if_stmt->then_block, if_stmt->else_block);
    anvil_func_set_current_block(fn, if_stmt->then_block);
    
    return if_stmt;
}

void anvil_build_if_else(AnvilIf* if_stmt) {
    AnvilFunc* fn = if_stmt->then_block->func;
    
    if (!anvil_block_is_terminated(fn->current)) {
        anvil_build_br(fn, if_stmt->merge_block);
    }
    
    if_stmt->in_else = true;
    anvil_func_set_current_block(fn, if_stmt->else_block);
}

void anvil_build_if_end(AnvilIf* if_stmt) {
    AnvilFunc* fn = if_stmt->then_block->func;
    
    if (!anvil_block_is_terminated(fn->current)) {
        anvil_build_br(fn, if_stmt->merge_block);
    }
    
    if (!if_stmt->in_else) {
        anvil_func_set_current_block(fn, if_stmt->else_block);
        anvil_build_br(fn, if_stmt->merge_block);
    }
    
    anvil_func_set_current_block(fn, if_stmt->merge_block);
    anvil_vec_pop(&fn->if_stack);
}

AnvilLoop* anvil_build_while_begin(AnvilFunc* fn, AnvilValue* cond) {
    AnvilLoop* loop = (AnvilLoop*)anvil_vec_push(&fn->loop_stack);
    
    loop->header = anvil_func_add_block(fn, "while.header");
    loop->body = anvil_func_add_block(fn, "while.body");
    loop->exit = anvil_func_add_block(fn, "while.end");
    loop->var = NULL;
    loop->end_val = NULL;
    loop->step_val = NULL;
    
    anvil_build_br(fn, loop->header);
    anvil_func_set_current_block(fn, loop->header);
    anvil_build_br_cond(fn, cond, loop->body, loop->exit);
    anvil_func_set_current_block(fn, loop->body);
    
    return loop;
}

void anvil_build_while_end(AnvilLoop* loop) {
    AnvilFunc* fn = loop->header->func;
    
    if (!anvil_block_is_terminated(fn->current)) {
        anvil_build_br(fn, loop->header);
    }
    
    anvil_func_set_current_block(fn, loop->exit);
    anvil_vec_pop(&fn->loop_stack);
}

AnvilLoop* anvil_build_for_begin(AnvilFunc* fn, AnvilVar* var, AnvilValue* start, AnvilValue* end, AnvilValue* step) {
    anvil_build_store(fn, var, start);
    
    AnvilLoop* loop = (AnvilLoop*)anvil_vec_push(&fn->loop_stack);
    
    loop->header = anvil_func_add_block(fn, "for.header");
    loop->body = anvil_func_add_block(fn, "for.body");
    loop->exit = anvil_func_add_block(fn, "for.end");
    loop->var = var;
    loop->end_val = end;
    loop->step_val = step;
    
    anvil_build_br(fn, loop->header);
    anvil_func_set_current_block(fn, loop->header);
    
    AnvilValue* current = anvil_build_load(fn, var);
    AnvilValue* cond = anvil_build_lt(fn, current, end);
    anvil_build_br_cond(fn, cond, loop->body, loop->exit);
    anvil_func_set_current_block(fn, loop->body);
    
    return loop;
}

void anvil_build_for_end(AnvilLoop* loop) {
    AnvilFunc* fn = loop->header->func;
    
    if (!anvil_block_is_terminated(fn->current)) {
        AnvilValue* current = anvil_build_load(fn, loop->var);
        AnvilValue* next = anvil_build_add(fn, current, loop->step_val);
        anvil_build_store(fn, loop->var, next);
        anvil_build_br(fn, loop->header);
    }
    
    anvil_func_set_current_block(fn, loop->exit);
    anvil_vec_pop(&fn->loop_stack);
}

void anvil_build_break(AnvilFunc* fn) {
    AnvilLoop* loop = (AnvilLoop*)anvil_vec_last(&fn->loop_stack);
    if (loop) {
        anvil_build_br(fn, loop->exit);
    }
}

void anvil_build_continue(AnvilFunc* fn) {
    AnvilLoop* loop = (AnvilLoop*)anvil_vec_last(&fn->loop_stack);
    if (loop) {
        anvil_build_br(fn, loop->header);
    }
}

AnvilValue* anvil_build_call(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, int num_args, AnvilValue** args) {
    return anvil_build_call_ex(fn, func_name, ret_type, num_args, args, false, num_args);
}

AnvilValue* anvil_build_call_ex(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, 
                                 int num_args, AnvilValue** args, bool is_variadic, int num_fixed_args) {
    AnvilValue* result = NULL;
    if (!anvil_type_is_void(ret_type)) {
        result = anvil_func_new_temp(fn, ret_type);
    }
    
    AnvilInst* inst = anvil_inst_call(fn->arena, func_name, result);
    inst->call.is_variadic = is_variadic;
    inst->call.num_fixed_args = num_fixed_args;
    for (int i = 0; i < num_args; i++) {
        anvil_inst_add_arg(inst, args[i]);
    }
    anvil_func_emit(fn, inst);
    return result;
}

AnvilValue* anvil_build_call_indirect(AnvilFunc* fn, AnvilValue* func_ptr, AnvilType* func_type, int num_args, AnvilValue** args) {
    AnvilType* ret_type = func_type->func.ret_type;
    AnvilValue* result = NULL;
    if (!anvil_type_is_void(ret_type)) {
        result = anvil_func_new_temp(fn, ret_type);
    }
    
    AnvilInst* inst = anvil_inst_create(fn->arena, ANVIL_INST_CALL_INDIRECT);
    inst->operands[0] = func_ptr;
    inst->num_operands = 1;
    inst->result = result;
    anvil_vec_init(&inst->call.args, sizeof(AnvilValue*));
    
    for (int i = 0; i < num_args; i++) {
        anvil_inst_add_arg(inst, args[i]);
    }
    anvil_func_emit(fn, inst);
    return result;
}
