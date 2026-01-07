#ifndef ANVIL_BUILDER_H
#define ANVIL_BUILDER_H

#include "func.h"
#include "value.h"
#include "inst.h"

#ifdef __cplusplus
extern "C" {
#endif

AnvilValue* anvil_build_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_mod(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_neg(AnvilFunc* fn, AnvilValue* val);

AnvilValue* anvil_build_and(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_or(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_xor(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_not(AnvilFunc* fn, AnvilValue* val);
AnvilValue* anvil_build_shl(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);
AnvilValue* anvil_build_shr(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);
AnvilValue* anvil_build_sar(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);

AnvilValue* anvil_build_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_ne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_le(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_gt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_build_ge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);

AnvilValue* anvil_build_load(AnvilFunc* fn, AnvilVar* var);
void anvil_build_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val);
AnvilValue* anvil_build_addr_of(AnvilFunc* fn, AnvilVar* var);
AnvilValue* anvil_build_deref(AnvilFunc* fn, AnvilValue* ptr);
AnvilValue* anvil_build_index(AnvilFunc* fn, AnvilValue* ptr, AnvilValue* idx);
AnvilValue* anvil_build_field(AnvilFunc* fn, AnvilValue* struct_ptr, const char* field);

AnvilValue* anvil_build_cast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_build_bitcast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_build_trunc(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_build_zext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_build_sext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);

void anvil_build_ret(AnvilFunc* fn, AnvilValue* val);
void anvil_build_ret_void(AnvilFunc* fn);
void anvil_build_br(AnvilFunc* fn, AnvilBlock* target);
void anvil_build_br_cond(AnvilFunc* fn, AnvilValue* cond, AnvilBlock* then_bb, AnvilBlock* else_bb);

AnvilIf* anvil_build_if_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_build_if_else(AnvilIf* if_stmt);
void anvil_build_if_end(AnvilIf* if_stmt);

AnvilLoop* anvil_build_while_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_build_while_end(AnvilLoop* loop);

AnvilLoop* anvil_build_for_begin(AnvilFunc* fn, AnvilVar* var, AnvilValue* start, AnvilValue* end, AnvilValue* step);
void anvil_build_for_end(AnvilLoop* loop);

void anvil_build_break(AnvilFunc* fn);
void anvil_build_continue(AnvilFunc* fn);

AnvilValue* anvil_build_call(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, int num_args, AnvilValue** args);
AnvilValue* anvil_build_call_ex(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, 
                                 int num_args, AnvilValue** args, bool is_variadic, int num_fixed_args);
AnvilValue* anvil_build_call_indirect(AnvilFunc* fn, AnvilValue* func_ptr, AnvilType* func_type, int num_args, AnvilValue** args);

#ifdef __cplusplus
}
#endif

#endif
