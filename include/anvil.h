#ifndef ANVIL_H
#define ANVIL_H

#include "anvil/types.h"
#include "anvil/target.h"
#include "anvil/result.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void anvil_init(void);
void anvil_shutdown(void);

AnvilModule* anvil_module_new(const char* name);
void anvil_module_free(AnvilModule* mod);

AnvilFunc* anvil_func_new(AnvilModule* mod, const char* name, AnvilType* ret_type);
AnvilVar* anvil_func_add_param(AnvilFunc* fn, const char* name, AnvilType* type);
AnvilVar* anvil_func_add_local(AnvilFunc* fn, const char* name, AnvilType* type);

AnvilValue* anvil_const_bool(AnvilFunc* fn, bool val);
AnvilValue* anvil_const_i8(AnvilFunc* fn, int8_t val);
AnvilValue* anvil_const_i16(AnvilFunc* fn, int16_t val);
AnvilValue* anvil_const_i32(AnvilFunc* fn, int32_t val);
AnvilValue* anvil_const_i64(AnvilFunc* fn, int64_t val);
AnvilValue* anvil_const_u8(AnvilFunc* fn, uint8_t val);
AnvilValue* anvil_const_u16(AnvilFunc* fn, uint16_t val);
AnvilValue* anvil_const_u32(AnvilFunc* fn, uint32_t val);
AnvilValue* anvil_const_u64(AnvilFunc* fn, uint64_t val);
AnvilValue* anvil_const_f32(AnvilFunc* fn, float val);
AnvilValue* anvil_const_f64(AnvilFunc* fn, double val);
AnvilValue* anvil_const_null(AnvilFunc* fn, AnvilType* ptr_type);
AnvilValue* anvil_const_string(AnvilModule* mod, const char* str);

AnvilValue* anvil_fadd(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fsub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fmul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fdiv(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fneg(AnvilFunc* fn, AnvilValue* val);

AnvilValue* anvil_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_mod(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_neg(AnvilFunc* fn, AnvilValue* val);

AnvilValue* anvil_and(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_or(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_xor(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_not(AnvilFunc* fn, AnvilValue* val);
AnvilValue* anvil_shl(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);
AnvilValue* anvil_shr(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);
AnvilValue* anvil_sar(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount);

AnvilValue* anvil_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_ne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_le(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_gt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_ge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);

AnvilValue* anvil_feq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_flt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fle(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fgt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_fge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);

AnvilValue* anvil_load(AnvilFunc* fn, AnvilVar* var);
void anvil_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val);
AnvilValue* anvil_addr_of(AnvilFunc* fn, AnvilVar* var);
AnvilValue* anvil_deref(AnvilFunc* fn, AnvilValue* ptr);
AnvilValue* anvil_index(AnvilFunc* fn, AnvilValue* ptr, AnvilValue* idx);
AnvilValue* anvil_field(AnvilFunc* fn, AnvilValue* struct_ptr, const char* field);

AnvilValue* anvil_cast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_bitcast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_trunc(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_zext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_sext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_fpext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_fptrunc(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_fptosi(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_fptoui(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_sitofp(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);
AnvilValue* anvil_uitofp(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type);

void anvil_ret(AnvilFunc* fn, AnvilValue* val);
void anvil_ret_void(AnvilFunc* fn);

AnvilIf* anvil_if_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_if_else(AnvilIf* if_stmt);
void anvil_if_end(AnvilIf* if_stmt);

AnvilLoop* anvil_while_begin(AnvilFunc* fn, AnvilValue* cond);
void anvil_while_end(AnvilLoop* loop);

AnvilLoop* anvil_for_begin(AnvilFunc* fn, AnvilVar* var, AnvilValue* start, 
                            AnvilValue* end, AnvilValue* step);
void anvil_for_end(AnvilLoop* loop);

void anvil_break(AnvilFunc* fn);
void anvil_continue(AnvilFunc* fn);

AnvilValue* anvil_call(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, int num_args, ...);
AnvilValue* anvil_call_variadic(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, 
                                 int num_fixed_args, int num_args, ...);
AnvilValue* anvil_call_indirect(AnvilFunc* fn, AnvilValue* func_ptr, 
                                 AnvilType* func_type, int num_args, ...);

void anvil_declare_extern(AnvilModule* mod, const char* name, AnvilType* func_type);

AnvilCompileResult anvil_compile(AnvilModule* mod, AnvilTarget target, int opt_level);

void anvil_module_dump_ir(AnvilModule* mod, FILE* out);
const char* anvil_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
