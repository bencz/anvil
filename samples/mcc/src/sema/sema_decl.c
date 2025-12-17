/*
 * MCC - Micro C Compiler
 * Semantic Analysis - Declaration Analysis
 * 
 * This file handles semantic analysis of declarations (functions, variables, etc.).
 */

#include "sema_internal.h"

/* ============================================================
 * Function Declaration Analysis
 * ============================================================ */

bool sema_analyze_func_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    /* Create function type */
    mcc_func_param_t *params = NULL;
    mcc_func_param_t *param_tail = NULL;
    
    for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
        mcc_ast_node_t *p = decl->data.func_decl.params[i];
        mcc_func_param_t *param = mcc_alloc(sema->ctx, sizeof(mcc_func_param_t));
        param->name = p->data.param_decl.name;
        param->type = p->data.param_decl.param_type;
        param->next = NULL;
        
        if (!params) params = param;
        if (param_tail) param_tail->next = param;
        param_tail = param;
    }
    
    mcc_type_t *func_type = mcc_type_function(sema->types,
        decl->data.func_decl.func_type,
        params,
        (int)decl->data.func_decl.num_params,
        decl->data.func_decl.is_variadic);
    
    /* Check for implicit int return type (C89 only) */
    if (!decl->data.func_decl.func_type) {
        if (sema_has_implicit_int(sema)) {
            mcc_warning_at(sema->ctx, decl->location,
                           "implicit int return type is deprecated");
            decl->data.func_decl.func_type = mcc_type_int(sema->types);
        } else {
            mcc_error_at(sema->ctx, decl->location,
                         "missing return type (implicit int not allowed in C99+)");
        }
    }
    
    /* Define function symbol */
    mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
        decl->data.func_decl.name,
        SYM_FUNC,
        func_type,
        decl->location);
    
    if (sym && decl->data.func_decl.is_definition) {
        if (sym->is_defined) {
            mcc_error_at(sema->ctx, decl->location,
                         "redefinition of function '%s'", decl->data.func_decl.name);
            return false;
        }
        
        sym->is_defined = true;
        sym->ast_node = decl;
        
        /* Analyze function body */
        sema->current_func = sym;
        sema->current_return_type = decl->data.func_decl.func_type;
        
        mcc_symtab_push_function_scope(sema->symtab);
        
        /* Define parameters */
        for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
            mcc_ast_node_t *p = decl->data.func_decl.params[i];
            if (p->data.param_decl.name) {
                mcc_symtab_define(sema->symtab,
                    p->data.param_decl.name,
                    SYM_PARAM,
                    p->data.param_decl.param_type,
                    p->location);
            }
        }
        
        sema_analyze_stmt(sema, decl->data.func_decl.body);
        
        /* TODO: Check that all labels referenced by goto are defined */
        
        mcc_symtab_pop_scope(sema->symtab);
        
        sema->current_func = NULL;
        sema->current_return_type = NULL;
    }
    
    return true;
}

/* ============================================================
 * Variable Declaration Analysis
 * ============================================================ */

bool sema_analyze_var_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    mcc_type_t *var_type = decl->data.var_decl.var_type;
    
    /* Check for complete type */
    if (!sema_check_complete_type(sema, var_type, decl->location)) {
        /* Allow incomplete array with initializer */
        if (!mcc_type_is_array(var_type) || !decl->data.var_decl.init) {
            mcc_error_at(sema->ctx, decl->location,
                         "variable has incomplete type");
        }
    }
    
    /* Check for void type */
    if (mcc_type_is_void(var_type)) {
        mcc_error_at(sema->ctx, decl->location,
                     "variable has void type");
        return false;
    }
    
    /* Define variable symbol */
    mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
        decl->data.var_decl.name,
        SYM_VAR,
        var_type,
        decl->location);
    
    if (sym && decl->data.var_decl.init) {
        mcc_type_t *init_type = sema_analyze_expr(sema, decl->data.var_decl.init);
        if (init_type) {
            sema_check_assignment_compat(sema, var_type, init_type, decl->location);
        }
    }
    
    return true;
}

/* ============================================================
 * Typedef Declaration Analysis
 * ============================================================ */

static bool analyze_typedef_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    mcc_type_t *type = decl->data.typedef_decl.type;
    
    /* If the typedef is for an enum, register the enum constants */
    if (type && type->kind == TYPE_ENUM && type->data.enumeration.is_complete) {
        mcc_type_t *int_type = mcc_type_int(sema->types);
        for (mcc_enum_const_t *c = type->data.enumeration.constants; c; c = c->next) {
            mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
                c->name,
                SYM_ENUM_CONST,
                int_type,
                decl->location);
            if (sym) {
                sym->data.enum_value = (int)c->value;
            }
        }
    }
    
    mcc_symtab_define(sema->symtab,
        decl->data.typedef_decl.name,
        SYM_TYPEDEF,
        type,
        decl->location);
    return true;
}

/* ============================================================
 * Struct/Union Declaration Analysis
 * ============================================================ */

static bool analyze_struct_decl(mcc_sema_t *sema, mcc_ast_node_t *decl, bool is_union)
{
    /* Register the tag in symbol table if named */
    if (decl->data.struct_decl.tag && decl->data.struct_decl.struct_type) {
        mcc_symtab_define_tag(sema->symtab,
            decl->data.struct_decl.tag,
            is_union ? SYM_UNION : SYM_STRUCT,
            decl->data.struct_decl.struct_type,
            decl->location);
    }
    return true;
}

/* ============================================================
 * Enum Declaration Analysis
 * ============================================================ */

static bool analyze_enum_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    /* Get the enum type from the declaration */
    mcc_type_t *enum_type = decl->data.enum_decl.enum_type;
    if (!enum_type) return true;
    
    /* Register the enum tag in symbol table if named */
    if (decl->data.enum_decl.tag) {
        mcc_symtab_define_tag(sema->symtab,
            decl->data.enum_decl.tag,
            SYM_ENUM,
            enum_type,
            decl->location);
    }
    
    /* Register each enum constant in the symbol table */
    mcc_type_t *int_type = mcc_type_int(sema->types);
    for (mcc_enum_const_t *c = enum_type->data.enumeration.constants; c; c = c->next) {
        mcc_symbol_t *sym = mcc_symtab_define(sema->symtab,
            c->name,
            SYM_ENUM_CONST,
            int_type,
            decl->location);
        if (sym) {
            sym->data.enum_value = (int)c->value;
        }
    }
    
    return true;
}

/* ============================================================
 * Static Assert Analysis (C11)
 * ============================================================ */

static bool analyze_static_assert(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    if (!sema_has_static_assert(sema)) {
        mcc_error_at(sema->ctx, decl->location,
                     "_Static_assert requires C11 or later");
        return false;
    }
    
    int64_t result;
    if (!sema_eval_const_expr(sema, decl->data.static_assert.expr, &result)) {
        mcc_error_at(sema->ctx, decl->location,
                     "static assertion expression is not constant");
        return false;
    }
    
    if (!result) {
        if (decl->data.static_assert.message) {
            mcc_error_at(sema->ctx, decl->location,
                         "static assertion failed: %s",
                         decl->data.static_assert.message);
        } else {
            mcc_error_at(sema->ctx, decl->location,
                         "static assertion failed");
        }
        return false;
    }
    
    return true;
}

/* ============================================================
 * Main Declaration Analysis Entry Point
 * ============================================================ */

bool sema_analyze_decl(mcc_sema_t *sema, mcc_ast_node_t *decl)
{
    if (!decl) return true;
    
    switch (decl->kind) {
        case AST_FUNC_DECL:
            return sema_analyze_func_decl(sema, decl);
            
        case AST_VAR_DECL:
            return sema_analyze_var_decl(sema, decl);
            
        case AST_DECL_LIST: {
            /* Multiple declarations: int a, b, c; */
            bool success = true;
            for (size_t i = 0; i < decl->data.decl_list.num_decls; i++) {
                if (!sema_analyze_decl(sema, decl->data.decl_list.decls[i])) {
                    success = false;
                }
            }
            return success;
        }
            
        case AST_TYPEDEF_DECL:
            return analyze_typedef_decl(sema, decl);
            
        case AST_STRUCT_DECL:
            return analyze_struct_decl(sema, decl, false);
            
        case AST_UNION_DECL:
            return analyze_struct_decl(sema, decl, true);
            
        case AST_ENUM_DECL:
            return analyze_enum_decl(sema, decl);
            
        case AST_STATIC_ASSERT:
            return analyze_static_assert(sema, decl);
            
        default:
            return true;
    }
}
