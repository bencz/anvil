/*
 * MCC - Micro C Compiler
 * Code Generator - Declaration Generation
 * 
 * This file handles code generation for declarations (functions, global variables).
 */

#include "codegen_internal.h"
#include <limits.h>

static anvil_value_t *codegen_zero_initializer(mcc_codegen_t *cg,
                                                mcc_type_t *type)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    if (!type) return NULL;

    anvil_type_t *anvil_type = codegen_type(cg, type);
    if (mcc_type_is_integer(type))
        return codegen_const_int_for_type(cg, anvil_type, 0);
    if (mcc_type_is_floating(type))
        return anvil_type_size(anvil_type) == 4
            ? anvil_const_f32(cg->anvil_ctx, 0.0f)
            : anvil_const_f64(cg->anvil_ctx, 0.0);
    if (mcc_type_is_pointer(type))
        return anvil_const_null(cg->anvil_ctx, anvil_type);
    if (type->kind == TYPE_ARRAY) {
        size_t count = type->data.array.length;
        if (count > SIZE_MAX / sizeof(anvil_value_t *)) {
            mcc_error(cg->mcc_ctx, "initializer element table overflow");
            return NULL;
        }
        anvil_value_t **elements = count
            ? mcc_alloc(cg->mcc_ctx, count * sizeof(*elements)) : NULL;
        if (count && !elements) return NULL;
        for (size_t i = 0; i < count; i++) {
            elements[i] = codegen_zero_initializer(cg, type->data.array.element);
            if (!elements[i]) return NULL;
        }
        return anvil_const_array(cg->anvil_ctx,
                                 codegen_type(cg, type->data.array.element),
                                 elements, count);
    }
    return NULL;
}

static bool codegen_eval_initializer_int(mcc_ast_node_t *expr, uint64_t *value)
{
    if (!expr || !value) return false;
    if (expr->kind == AST_INT_LIT) {
        *value = expr->data.int_lit.value;
        return true;
    }
    if (expr->kind == AST_CHAR_LIT) {
        *value = (uint64_t)(int64_t)expr->data.char_lit.value;
        return true;
    }
    if (expr->kind == AST_UNARY_EXPR) {
        uint64_t operand;
        if (!codegen_eval_initializer_int(expr->data.unary_expr.operand,
                                          &operand)) return false;
        switch (expr->data.unary_expr.op) {
            case UNOP_POS:     *value = operand; return true;
            case UNOP_NEG:     *value = UINT64_C(0) - operand; return true;
            case UNOP_NOT:     *value = !operand; return true;
            case UNOP_BIT_NOT: *value = ~operand; return true;
            default: return false;
        }
    }
    return false;
}

static anvil_value_t *codegen_global_initializer(mcc_codegen_t *cg,
                                                  mcc_type_t *type,
                                                  mcc_ast_node_t *init)
{
    while (type && type->kind == TYPE_TYPEDEF)
        type = type->data.typedef_ref.underlying;
    if (!type || !init) return NULL;

    anvil_type_t *anvil_type = codegen_type(cg, type);
    if (type->kind == TYPE_ARRAY) {
        size_t count = type->data.array.length;
        if (count > SIZE_MAX / sizeof(anvil_value_t *)) {
            mcc_error(cg->mcc_ctx, "initializer element table overflow");
            return NULL;
        }
        if (init->kind == AST_STRING_LIT &&
            type->data.array.element->kind == TYPE_CHAR) {
            size_t string_len = strlen(init->data.string_lit.value);
            if (string_len > count) return NULL;
            anvil_type_t *elem_type = codegen_type(cg,
                                                   type->data.array.element);
            anvil_value_t **elements = count
                ? mcc_alloc(cg->mcc_ctx, count * sizeof(*elements)) : NULL;
            if (count && !elements) return NULL;
            for (size_t i = 0; i < count; i++) {
                unsigned char byte = i < string_len
                    ? (unsigned char)init->data.string_lit.value[i] : 0;
                elements[i] = codegen_const_int_for_type(cg, elem_type, byte);
                if (!elements[i]) return NULL;
            }
            return anvil_const_array(cg->anvil_ctx, elem_type,
                                     elements, count);
        }
        if (init->kind != AST_INIT_LIST) return NULL;
        if (init->data.init_list.num_exprs > count) return NULL;
        anvil_value_t **elements = count
            ? mcc_alloc(cg->mcc_ctx, count * sizeof(*elements)) : NULL;
        if (count && !elements) return NULL;
        for (size_t i = 0; i < count; i++) {
            elements[i] = i < init->data.init_list.num_exprs
                ? codegen_global_initializer(cg, type->data.array.element,
                                             init->data.init_list.exprs[i])
                : codegen_zero_initializer(cg, type->data.array.element);
            if (!elements[i]) return NULL;
        }
        return anvil_const_array(cg->anvil_ctx,
                                 codegen_type(cg, type->data.array.element),
                                 elements, count);
    }

    if (mcc_type_is_integer(type)) {
        uint64_t value;
        if (codegen_eval_initializer_int(init, &value))
            return codegen_const_int_for_type(cg, anvil_type, (int64_t)value);
        if (init->kind == AST_SIZEOF_EXPR) {
            mcc_type_t *arg = init->data.sizeof_expr.type_arg;
            if (!arg && init->data.sizeof_expr.expr_arg)
                arg = init->data.sizeof_expr.expr_arg->type;
            if (arg)
                return codegen_const_int_for_type(cg, anvil_type,
                                                  (int64_t)mcc_type_sizeof(arg));
        }
        if (init->kind == AST_ALIGNOF_EXPR) {
            mcc_type_t *arg = init->data.alignof_expr.type_arg;
            if (!arg && init->data.alignof_expr.expr_arg)
                arg = init->data.alignof_expr.expr_arg->type;
            if (arg)
                return codegen_const_int_for_type(
                    cg, anvil_type,
                    (int64_t)mcc_type_alignof(arg));
        }
    }
    if (mcc_type_is_floating(type) && init->kind == AST_FLOAT_LIT) {
        return anvil_type_size(anvil_type) == 4
            ? anvil_const_f32(cg->anvil_ctx, (float)init->data.float_lit.value)
            : anvil_const_f64(cg->anvil_ctx, init->data.float_lit.value);
    }
    if (mcc_type_is_pointer(type) && init->kind == AST_NULL_PTR)
        return anvil_const_null(cg->anvil_ctx, anvil_type);
    if (mcc_type_is_pointer(type) && init->kind == AST_INT_LIT &&
        init->data.int_lit.value == 0)
        return anvil_const_null(cg->anvil_ctx, anvil_type);
    if (mcc_type_is_pointer(type) && init->kind == AST_STRING_LIT)
        return codegen_get_string_literal(cg, init->data.string_lit.value);
    return NULL;
}

/* Generate code for function */
void codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func)
{
    if (!func->data.func_decl.is_definition) {
        /* Just a declaration, no code to generate */
        return;
    }
    
    /* Create function type */
    anvil_type_t *ret_type = codegen_type(cg, func->data.func_decl.func_type);
    if (!ret_type || func->data.func_decl.num_params > (size_t)INT_MAX ||
        func->data.func_decl.num_params >
            SIZE_MAX / sizeof(anvil_type_t *)) {
        if (ret_type) mcc_error(cg->mcc_ctx,
                                "function parameter table overflow");
        return;
    }
    int num_params = (int)func->data.func_decl.num_params;
    anvil_type_t **param_types = NULL;
    
    if (num_params > 0) {
        param_types = mcc_alloc(cg->mcc_ctx, num_params * sizeof(anvil_type_t*));
        if (!param_types) return;
        for (int i = 0; i < num_params; i++) {
            mcc_ast_node_t *p = func->data.func_decl.params[i];
            param_types[i] = codegen_param_type(cg, p->data.param_decl.param_type);
            if (!param_types[i]) return;
        }
    }
    
    anvil_type_t *func_type = anvil_type_func(cg->anvil_ctx, ret_type, param_types,
                                               num_params, false);
    if (!func_type) return;
    
    /* Create function */
    anvil_linkage_t linkage = func->data.func_decl.is_static ? 
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    cg->current_func = anvil_func_create(cg->anvil_mod, func->data.func_decl.name, 
                                          func_type, linkage);
    cg->current_func_name = func->data.func_decl.name;  /* For __func__ (C99) */
    cg->current_return_type = func->data.func_decl.func_type;
    
    /* Register function in mapping */
    mcc_symbol_t *func_sym = mcc_symtab_lookup(cg->symtab, func->data.func_decl.name);
    if (func_sym) {
        codegen_add_func(cg, func_sym, cg->current_func);
    }
    
    /* Create entry block */
    anvil_block_t *entry = anvil_func_get_entry(cg->current_func);
    codegen_set_current_block(cg, entry);
    
    /* Reset locals */
    cg->num_locals = 0;
    cg->num_labels = 0;
    
    /* Allocate space for parameters and add to locals */
    for (int i = 0; i < num_params; i++) {
        mcc_ast_node_t *p = func->data.func_decl.params[i];
        if (p->data.param_decl.name) {
            anvil_value_t *param = anvil_func_get_param(cg->current_func, i);
            if (codegen_type_pass_by_reference(p->data.param_decl.param_type)) {
                codegen_add_local(cg, p->data.param_decl.name, param);
                continue;
            }
            anvil_value_t *alloca_val = anvil_build_alloca(cg->anvil_ctx, param_types[i], 
                                                           p->data.param_decl.name);
            anvil_build_store(cg->anvil_ctx, param, alloca_val);
            
            /* Add parameter to locals by name */
            codegen_add_local(cg, p->data.param_decl.name, alloca_val);
        }
    }
    
    /* Generate body */
    codegen_stmt(cg, func->data.func_decl.body);
    
    /* Add implicit return if needed */
    if (!codegen_block_has_terminator(cg)) {
        if (ret_type == anvil_type_void(cg->anvil_ctx)) {
            anvil_build_ret_void(cg->anvil_ctx);
        } else {
            anvil_value_t *zero = anvil_const_i32(cg->anvil_ctx, 0);
            anvil_build_ret(cg->anvil_ctx, zero);
        }
    }
    
    cg->current_func = NULL;
    cg->current_block = NULL;
    cg->current_return_type = NULL;
}

/* Generate code for global variable */
void codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var)
{
    anvil_type_t *type = codegen_type(cg, var->data.var_decl.var_type);
    
    /* Use cache to avoid duplicate global definitions */
    anvil_value_t *global = codegen_get_or_add_global(cg, var->data.var_decl.name, type);
    
    mcc_ast_node_t *init = var->data.var_decl.init;
    if (init) {
        anvil_value_t *constant = codegen_global_initializer(
            cg, var->data.var_decl.var_type, init);
        if (!constant) {
            mcc_error_at(cg->mcc_ctx, init->location,
                         "unsupported or invalid constant initializer for global '%s'",
                         var->data.var_decl.name);
            return;
        }
        anvil_global_set_initializer(global, constant);
    }
}

/* Generate code for any declaration */
void codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl)
{
    if (!decl) return;
    
    switch (decl->kind) {
        case AST_FUNC_DECL:
            codegen_func(cg, decl);
            break;
        case AST_VAR_DECL:
            codegen_global_var(cg, decl);
            break;
        case AST_DECL_LIST:
            /* Multiple declarations: int a, b, c; */
            for (size_t i = 0; i < decl->data.decl_list.num_decls; i++) {
                codegen_decl(cg, decl->data.decl_list.decls[i]);
            }
            break;
        default:
            break;
    }
}
