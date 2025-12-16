/*
 * MCC - Micro C Compiler
 * Code Generator - Declaration Generation
 * 
 * This file handles code generation for declarations (functions, global variables).
 */

#include "codegen_internal.h"

/* Generate code for function */
void codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func)
{
    if (!func->data.func_decl.is_definition) {
        /* Just a declaration, no code to generate */
        return;
    }
    
    /* Create function type */
    anvil_type_t *ret_type = codegen_type(cg, func->data.func_decl.func_type);
    int num_params = (int)func->data.func_decl.num_params;
    anvil_type_t **param_types = NULL;
    
    if (num_params > 0) {
        param_types = mcc_alloc(cg->mcc_ctx, num_params * sizeof(anvil_type_t*));
        for (int i = 0; i < num_params; i++) {
            mcc_ast_node_t *p = func->data.func_decl.params[i];
            param_types[i] = codegen_type(cg, p->data.param_decl.param_type);
        }
    }
    
    anvil_type_t *func_type = anvil_type_func(cg->anvil_ctx, ret_type, param_types,
                                               num_params, false);
    
    /* Create function */
    anvil_linkage_t linkage = func->data.func_decl.is_static ? 
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    cg->current_func = anvil_func_create(cg->anvil_mod, func->data.func_decl.name, 
                                          func_type, linkage);
    cg->current_func_name = func->data.func_decl.name;  /* For __func__ (C99) */
    
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
}

/* Generate code for global variable */
void codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var)
{
    anvil_type_t *type = codegen_type(cg, var->data.var_decl.var_type);
    
    anvil_linkage_t linkage = var->data.var_decl.is_static ?
        ANVIL_LINK_INTERNAL : ANVIL_LINK_EXTERNAL;
    
    anvil_module_add_global(cg->anvil_mod, var->data.var_decl.name, type, linkage);
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
