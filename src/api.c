#include "../include/anvil.h"
#include "core/arena.h"
#include "core/error.h"
#include "core/str.h"
#include "ir/module.h"
#include "ir/func.h"
#include "ir/builder.h"
#include "ir/value.h"
#include "ir/types_internal.h"
#include "mir/mir.h"
#include "mir/lower.h"
#include "mir/regalloc.h"
#include "opt/ir_opt.h"
#include "opt/mir_opt.h"
#include "backend/backend.h"
#include "backend/x86_64/emit.h"
#include "backend/arm64/emit.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static bool anvil_initialized = false;

void anvil_init(void) {
    if (anvil_initialized) return;
    anvil_backends_init();
    anvil_initialized = true;
}

void anvil_shutdown(void) {
    anvil_initialized = false;
}

AnvilModule* anvil_module_new(const char* name) {
    anvil_init();
    return anvil_module_create(name);
}

void anvil_module_free(AnvilModule* mod) {
    anvil_module_destroy(mod);
}

AnvilType* anvil_type_ptr(AnvilModule* mod, AnvilType* pointee) {
    return anvil_type_ptr_create(mod->arena, pointee);
}

AnvilType* anvil_type_array(AnvilModule* mod, AnvilType* elem, int count) {
    return anvil_type_array_create(mod->arena, elem, (size_t)count);
}

AnvilFunc* anvil_func_new(AnvilModule* mod, const char* name, AnvilType* ret_type) {
    return anvil_module_add_func(mod, name, ret_type);
}

AnvilValue* anvil_const_bool(AnvilFunc* fn, bool val) {
    return anvil_value_const_bool(fn->arena, val);
}

AnvilValue* anvil_const_i8(AnvilFunc* fn, int8_t val) {
    return anvil_value_const_i8(fn->arena, val);
}

AnvilValue* anvil_const_i16(AnvilFunc* fn, int16_t val) {
    return anvil_value_const_i16(fn->arena, val);
}

AnvilValue* anvil_const_i32(AnvilFunc* fn, int32_t val) {
    return anvil_value_const_i32(fn->arena, val);
}

AnvilValue* anvil_const_i64(AnvilFunc* fn, int64_t val) {
    return anvil_value_const_i64(fn->arena, val);
}

AnvilValue* anvil_const_u8(AnvilFunc* fn, uint8_t val) {
    return anvil_value_const_u8(fn->arena, val);
}

AnvilValue* anvil_const_u16(AnvilFunc* fn, uint16_t val) {
    return anvil_value_const_u16(fn->arena, val);
}

AnvilValue* anvil_const_u32(AnvilFunc* fn, uint32_t val) {
    return anvil_value_const_u32(fn->arena, val);
}

AnvilValue* anvil_const_u64(AnvilFunc* fn, uint64_t val) {
    return anvil_value_const_u64(fn->arena, val);
}

AnvilValue* anvil_const_f32(AnvilFunc* fn, float val) {
    return anvil_value_const_f32(fn->arena, val);
}

AnvilValue* anvil_const_f64(AnvilFunc* fn, double val) {
    return anvil_value_const_f64(fn->arena, val);
}

AnvilValue* anvil_const_null(AnvilFunc* fn, AnvilType* ptr_type) {
    return anvil_value_const_null(fn->arena, ptr_type);
}

AnvilValue* anvil_const_string(AnvilModule* mod, const char* str) {
    return anvil_value_const_string(mod->arena, str);
}

AnvilValue* anvil_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_add(fn, lhs, rhs);
}

AnvilValue* anvil_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_sub(fn, lhs, rhs);
}

AnvilValue* anvil_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_mul(fn, lhs, rhs);
}

AnvilValue* anvil_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_div(fn, lhs, rhs);
}

AnvilValue* anvil_mod(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_mod(fn, lhs, rhs);
}

AnvilValue* anvil_neg(AnvilFunc* fn, AnvilValue* val) {
    return anvil_build_neg(fn, val);
}

AnvilValue* anvil_and(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_and(fn, lhs, rhs);
}

AnvilValue* anvil_or(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_or(fn, lhs, rhs);
}

AnvilValue* anvil_xor(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_xor(fn, lhs, rhs);
}

AnvilValue* anvil_not(AnvilFunc* fn, AnvilValue* val) {
    return anvil_build_not(fn, val);
}

AnvilValue* anvil_shl(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return anvil_build_shl(fn, val, amount);
}

AnvilValue* anvil_shr(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return anvil_build_shr(fn, val, amount);
}

AnvilValue* anvil_sar(AnvilFunc* fn, AnvilValue* val, AnvilValue* amount) {
    return anvil_build_sar(fn, val, amount);
}

AnvilValue* anvil_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_eq(fn, lhs, rhs);
}

AnvilValue* anvil_ne(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_ne(fn, lhs, rhs);
}

AnvilValue* anvil_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_lt(fn, lhs, rhs);
}

AnvilValue* anvil_le(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_le(fn, lhs, rhs);
}

AnvilValue* anvil_gt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_gt(fn, lhs, rhs);
}

AnvilValue* anvil_ge(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs) {
    return anvil_build_ge(fn, lhs, rhs);
}

AnvilValue* anvil_load(AnvilFunc* fn, AnvilVar* var) {
    return anvil_build_load(fn, var);
}

void anvil_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val) {
    anvil_build_store(fn, var, val);
}

AnvilValue* anvil_addr_of(AnvilFunc* fn, AnvilVar* var) {
    return anvil_build_addr_of(fn, var);
}

AnvilValue* anvil_deref(AnvilFunc* fn, AnvilValue* ptr) {
    return anvil_build_deref(fn, ptr);
}

AnvilValue* anvil_index(AnvilFunc* fn, AnvilValue* ptr, AnvilValue* idx) {
    return anvil_build_index(fn, ptr, idx);
}

AnvilValue* anvil_field(AnvilFunc* fn, AnvilValue* struct_ptr, const char* field_name) {
    return anvil_build_field(fn, struct_ptr, field_name);
}

AnvilValue* anvil_cast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    return anvil_build_cast(fn, val, to_type);
}

AnvilValue* anvil_bitcast(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    return anvil_build_bitcast(fn, val, to_type);
}

AnvilValue* anvil_trunc(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    return anvil_build_trunc(fn, val, to_type);
}

AnvilValue* anvil_zext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    return anvil_build_zext(fn, val, to_type);
}

AnvilValue* anvil_sext(AnvilFunc* fn, AnvilValue* val, AnvilType* to_type) {
    return anvil_build_sext(fn, val, to_type);
}

void anvil_ret(AnvilFunc* fn, AnvilValue* val) {
    anvil_build_ret(fn, val);
}

void anvil_ret_void(AnvilFunc* fn) {
    anvil_build_ret_void(fn);
}

AnvilIf* anvil_if_begin(AnvilFunc* fn, AnvilValue* cond) {
    return anvil_build_if_begin(fn, cond);
}

void anvil_if_else(AnvilIf* if_stmt) {
    anvil_build_if_else(if_stmt);
}

void anvil_if_end(AnvilIf* if_stmt) {
    anvil_build_if_end(if_stmt);
}

AnvilLoop* anvil_while_begin(AnvilFunc* fn, AnvilValue* cond) {
    return anvil_build_while_begin(fn, cond);
}

void anvil_while_end(AnvilLoop* loop) {
    anvil_build_while_end(loop);
}

AnvilLoop* anvil_for_begin(AnvilFunc* fn, AnvilVar* var, AnvilValue* start, 
                            AnvilValue* end, AnvilValue* step) {
    return anvil_build_for_begin(fn, var, start, end, step);
}

void anvil_for_end(AnvilLoop* loop) {
    anvil_build_for_end(loop);
}

void anvil_break(AnvilFunc* fn) {
    anvil_build_break(fn);
}

void anvil_continue(AnvilFunc* fn) {
    anvil_build_continue(fn);
}

AnvilValue* anvil_call(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, int num_args, ...) {
    AnvilValue** args = NULL;
    if (num_args > 0) {
        args = (AnvilValue**)malloc(sizeof(AnvilValue*) * num_args);
        va_list ap;
        va_start(ap, num_args);
        for (int i = 0; i < num_args; i++) {
            args[i] = va_arg(ap, AnvilValue*);
        }
        va_end(ap);
    }
    AnvilValue* result = anvil_build_call_ex(fn, func_name, ret_type, num_args, args, false, num_args);
    free(args);
    return result;
}

AnvilValue* anvil_call_variadic(AnvilFunc* fn, const char* func_name, AnvilType* ret_type, 
                                 int num_fixed_args, int num_args, ...) {
    AnvilValue** args = NULL;
    if (num_args > 0) {
        args = (AnvilValue**)malloc(sizeof(AnvilValue*) * num_args);
        va_list ap;
        va_start(ap, num_args);
        for (int i = 0; i < num_args; i++) {
            args[i] = va_arg(ap, AnvilValue*);
        }
        va_end(ap);
    }
    AnvilValue* result = anvil_build_call_ex(fn, func_name, ret_type, num_args, args, true, num_fixed_args);
    free(args);
    return result;
}

AnvilValue* anvil_call_indirect(AnvilFunc* fn, AnvilValue* func_ptr, 
                                 AnvilType* func_type, int num_args, ...) {
    AnvilValue** args = NULL;
    if (num_args > 0) {
        args = (AnvilValue**)malloc(sizeof(AnvilValue*) * num_args);
        va_list ap;
        va_start(ap, num_args);
        for (int i = 0; i < num_args; i++) {
            args[i] = va_arg(ap, AnvilValue*);
        }
        va_end(ap);
    }
    AnvilValue* result = anvil_build_call_indirect(fn, func_ptr, func_type, num_args, args);
    free(args);
    return result;
}

AnvilTarget anvil_target_x86_64_linux(void) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_X86_64;
    target.os = ANVIL_OS_LINUX;
    target.abi_name = "sysv";
    target.features = 0;
    return target;
}

AnvilTarget anvil_target_x86_64_windows(void) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_X86_64;
    target.os = ANVIL_OS_WINDOWS;
    target.abi_name = "win64";
    target.features = 0;
    return target;
}

AnvilTarget anvil_target_arm64_linux(void) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_ARM64;
    target.os = ANVIL_OS_LINUX;
    target.abi_name = "aapcs64";
    target.features = 0;
    return target;
}

AnvilTarget anvil_target_arm64_macos(void) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_ARM64;
    target.os = ANVIL_OS_MACOS;
    target.abi_name = "apple";
    target.features = 0;
    return target;
}

AnvilTarget anvil_target_ppc64_linux(void) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_PPC64;
    target.os = ANVIL_OS_LINUX;
    target.abi_name = "elfv2";
    target.features = 0;
    return target;
}

AnvilTarget anvil_target_from_triple(const char* triple) {
    AnvilTarget target;
    target.arch = ANVIL_ARCH_X86_64;
    target.os = ANVIL_OS_LINUX;
    target.abi_name = "sysv";
    target.features = 0;
    
    if (strstr(triple, "x86_64") || strstr(triple, "x86-64") || strstr(triple, "amd64")) {
        target.arch = ANVIL_ARCH_X86_64;
    } else if (strstr(triple, "aarch64") || strstr(triple, "arm64")) {
        target.arch = ANVIL_ARCH_ARM64;
    }
    
    if (strstr(triple, "linux")) {
        target.os = ANVIL_OS_LINUX;
        target.abi_name = (target.arch == ANVIL_ARCH_X86_64) ? "sysv" : "aapcs64";
    } else if (strstr(triple, "windows") || strstr(triple, "win32") || strstr(triple, "mingw")) {
        target.os = ANVIL_OS_WINDOWS;
        target.abi_name = "win64";
    } else if (strstr(triple, "darwin") || strstr(triple, "macos") || strstr(triple, "apple")) {
        target.os = ANVIL_OS_MACOS;
        target.abi_name = (target.arch == ANVIL_ARCH_ARM64) ? "apple" : "sysv";
    }
    
    return target;
}

AnvilCompileResult anvil_compile(AnvilModule* mod, AnvilTarget target, int opt_level) {
    AnvilCompileResult result = {0};
    
    AnvilBackend* backend = anvil_get_backend(target.arch);
    if (!backend) {
        result.errors = strdup("Unsupported target architecture");
        result.num_errors = 1;
        return result;
    }
    
    anvil_opt_run_all(mod, opt_level, NULL);
    
    const AnvilABI* abi = backend->get_abi ? backend->get_abi(target.os, target.abi_name) : backend->default_abi;
    
    AnvilMIR* mir = anvil_lower_module_with_abi(mod, abi);
    if (!mir) {
        result.errors = strdup("Failed to lower IR to MIR");
        result.num_errors = 1;
        return result;
    }
    
    anvil_mir_opt_run_all(mir, opt_level, NULL);
    
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        if (backend->isel) {
            backend->isel(backend, func);
        }
        
        if (backend->vectorize && opt_level >= ANVIL_OPT_AGGRESSIVE) {
            backend->vectorize(backend, func);
        }
        
        if (backend->regalloc) {
            backend->regalloc(backend, func, target.os, target.abi_name);
        }
        
        for (size_t bi = 0; bi < anvil_vec_len(&func->blocks); bi++) {
            AnvilMBlock* block = *(AnvilMBlock**)anvil_vec_get(&func->blocks, bi);
            if (backend->schedule_instructions && opt_level >= ANVIL_OPT_STANDARD) {
                backend->schedule_instructions(backend, block);
            }
        }
        
        if (backend->peephole_optimize) {
            backend->peephole_optimize(backend, func);
        }
    }
    
    AnvilAsmBuffer asm_buf;
    anvil_asm_init(&asm_buf);
    
    backend->emit_mir(backend, mir, &asm_buf, target.os, target.abi_name);
    
    result.code = anvil_asm_take(&asm_buf);
    result.length = result.code ? strlen(result.code) : 0;
    result.errors = NULL;
    result.num_errors = 0;
    
    return result;
}

void anvil_result_free(AnvilCompileResult* result) {
    if (result->code) {
        free(result->code);
        result->code = NULL;
    }
    if (result->errors) {
        free(result->errors);
        result->errors = NULL;
    }
    result->length = 0;
    result->num_errors = 0;
}

void anvil_module_dump_ir(AnvilModule* mod, FILE* out) {
    fprintf(out, "; Module: %s\n\n", mod->name);
    
    for (AnvilFunc* func = mod->first_func; func; func = func->next) {
        fprintf(out, "define %s @%s(", "type", func->name);
        
        for (size_t i = 0; i < anvil_vec_len(&func->params); i++) {
            if (i > 0) fprintf(out, ", ");
            AnvilVar** var = (AnvilVar**)anvil_vec_get(&func->params, i);
            fprintf(out, "%%%s", (*var)->name);
        }
        fprintf(out, ") {\n");
        
        for (AnvilBlock* block = func->entry; block; block = block->next) {
            fprintf(out, "%s:\n", block->name);
            for (AnvilInst* inst = block->first; inst; inst = inst->next) {
                fprintf(out, "  ; inst kind=%d\n", inst->kind);
            }
        }
        
        fprintf(out, "}\n\n");
    }
}

const char* anvil_get_last_error(void) {
    return NULL;
}

void anvil_declare_extern(AnvilModule* mod, const char* name, AnvilType* func_type) {
    (void)mod;
    (void)name;
    (void)func_type;
}
