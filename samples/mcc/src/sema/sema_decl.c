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
    decl->data.func_decl.func_type = sema_resolve_type(
        sema, decl->data.func_decl.func_type);
    /* Create function type */
    mcc_func_param_t *params = NULL;
    mcc_func_param_t *param_tail = NULL;
    
    for (size_t i = 0; i < decl->data.func_decl.num_params; i++) {
        mcc_ast_node_t *p = decl->data.func_decl.params[i];
        p->data.param_decl.param_type = sema_resolve_type(
            sema, p->data.param_decl.param_type);
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

        /* Before popping the function scope, walk the label namespace and
         * flag any label that was referenced by goto but never defined. */
        mcc_scope_t *fn_scope = mcc_symtab_current_scope(sema->symtab);
        while (fn_scope && !fn_scope->is_function_scope) {
            fn_scope = fn_scope->parent;
        }
        if (fn_scope && fn_scope->labels) {
            for (size_t i = 0; i < fn_scope->label_table_size; i++) {
                mcc_symbol_t *lbl = fn_scope->labels[i];
                for (; lbl; lbl = lbl->next) {
                    if (!lbl->is_defined) {
                        mcc_error_at(sema->ctx, lbl->location,
                            "label '%s' used but not defined", lbl->name);
                    }
                }
            }
        }

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
    var_type = sema_resolve_type(sema, var_type);
    if (!var_type) {
        mcc_error_at(sema->ctx, decl->location,
                     "could not resolve declaration type");
        return false;
    }
    decl->data.var_decl.var_type = var_type;
    
    /* Check for complete type. Exemptions:
     *  - an array with an initializer: the size is inferred from the init.
     *  - a VLA: the "length is 0" at compile time is expected; the size
     *    is determined at runtime by the length_expr. */
    if (!sema_check_complete_type(sema, var_type, decl->location)) {
        bool exempt = false;
        if (mcc_type_is_array(var_type)) {
            if (decl->data.var_decl.init) exempt = true;
            if (var_type->data.array.is_vla) exempt = true;
        }
        if (!exempt) {
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
    
    /* File-scope declarations and tentative definitions share one symbol.
     * Only a second initializer is a duplicate definition. Block locals keep
     * the ordinary same-scope redefinition checks. */
    bool file_scope = mcc_symtab_is_global_scope(sema->symtab);
    mcc_symbol_t *sym = file_scope ? mcc_symtab_lookup_current(sema->symtab, decl->data.var_decl.name) : NULL;
    if (sym && sym->kind == SYM_VAR)
    {
        if (!mcc_type_is_compatible(sym->type, var_type))
        {
            mcc_error_at(sema->ctx, decl->location, "conflicting types for variable '%s'", decl->data.var_decl.name);
            return false;
        }

        bool static_linkage = sym->storage == STORAGE_STATIC;
        if ((decl->data.var_decl.is_static && !static_linkage) ||
            (!decl->data.var_decl.is_static && !decl->data.var_decl.is_extern && static_linkage))
        {
            mcc_error_at(sema->ctx, decl->location, "conflicting linkage for variable '%s'", decl->data.var_decl.name);
            return false;
        }

        if (decl->data.var_decl.init && sym->is_defined)
        {
            mcc_error_at(sema->ctx, decl->location, "redefinition of variable '%s'", decl->data.var_decl.name);
            return false;
        }
    }
    else
    {
        sym = mcc_symtab_define(sema->symtab, decl->data.var_decl.name, SYM_VAR, var_type, decl->location);
        if (sym)
        {
            if (decl->data.var_decl.is_static)
                sym->storage = STORAGE_STATIC;
            else if (decl->data.var_decl.is_extern)
                sym->storage = STORAGE_EXTERN;
            else
                sym->storage = STORAGE_NONE;
        }
    }

    if (sym && decl->data.var_decl.init)
        sym->is_defined = true;
    
    if (sym && decl->data.var_decl.init) {
        mcc_type_t *init_type = sema_analyze_expr(sema, decl->data.var_decl.init);

        if (mcc_type_is_integer(var_type) &&
            decl->data.var_decl.init->kind != AST_INT_LIT &&
            decl->data.var_decl.init->kind != AST_CHAR_LIT) {
            int64_t value;
            if (sema_eval_const_expr(sema, decl->data.var_decl.init, &value)) {
                mcc_ast_node_t *init = decl->data.var_decl.init;
                init->kind = AST_INT_LIT;
                init->data.int_lit.value = (uint64_t)value;
                init->data.int_lit.suffix = INT_SUFFIX_NONE;
            }
        }

        /* Complete an array of unknown bound from its initializer.  Keeping a
         * zero-length surrogate here made codegen emit a zero-sized global
         * for perfectly ordinary declarations such as `char s[] = "x"`. */
        if (var_type->kind == TYPE_ARRAY &&
            var_type->data.array.length == 0 &&
            !var_type->data.array.is_vla) {
            mcc_ast_node_t *init = decl->data.var_decl.init;
            size_t length = 0;
            if (init->kind == AST_INIT_LIST) {
                length = init->data.init_list.num_exprs;
            } else if (init->kind == AST_STRING_LIT &&
                       var_type->data.array.element->kind == TYPE_CHAR) {
                length = strlen(init->data.string_lit.value) + 1;
            }
            if (length > 0) {
                var_type->data.array.length = length;
                var_type->size = var_type->data.array.element->size * length;
            }
        }
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
    type = sema_resolve_type(sema, type);
    decl->data.typedef_decl.type = type;
    
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

static bool analyze_static_assert(mcc_sema_t *sema, mcc_ast_node_t *decl);

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

    bool success = true;
    mcc_type_t *record = decl->data.struct_decl.struct_type;
    if (record) {
        for (mcc_record_assert_t *item = record->data.record.static_asserts;
             item; item = item->next) {
            mcc_ast_node_t assertion = { 0 };
            assertion.kind = AST_STATIC_ASSERT;
            assertion.location = item->location;
            assertion.data.static_assert_decl.expr = item->expr;
            assertion.data.static_assert_decl.message = item->message;
            if (!analyze_static_assert(sema, &assertion)) success = false;
        }
    }
    return success;
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

    if (!sema_analyze_expr(sema, decl->data.static_assert_decl.expr))
    {
        return false;
    }

    int64_t result;
    if (!sema_eval_const_expr(sema, decl->data.static_assert_decl.expr, &result))
    {
        mcc_error_at(sema->ctx, decl->location,
                     "static assertion expression is not constant");
        return false;
    }

    if (!result) {
        if (decl->data.static_assert_decl.message)
        {
            mcc_error_at(sema->ctx, decl->location, "static assertion failed: %s", decl->data.static_assert_decl.message);
        }
        else
        {
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
