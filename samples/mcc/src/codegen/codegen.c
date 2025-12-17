/*
 * MCC - Micro C Compiler
 * Code Generator - Main Module
 * 
 * This file contains the public API and core infrastructure
 * for the code generator.
 */

#include "codegen_internal.h"

/* ============================================================
 * Architecture Mapping
 * ============================================================ */

/* Use mcc_arch_to_anvil from context.c */

bool codegen_arch_is_darwin(mcc_arch_t arch)
{
    return arch == MCC_ARCH_ARM64_MACOS;
}

/* ============================================================
 * Public API - Create/Destroy
 * ============================================================ */

mcc_codegen_t *mcc_codegen_create(mcc_context_t *ctx, mcc_symtab_t *symtab,
                                   mcc_type_context_t *types)
{
    mcc_codegen_t *cg = mcc_alloc(ctx, sizeof(mcc_codegen_t));
    cg->mcc_ctx = ctx;
    cg->symtab = symtab;
    cg->types = types;
    
    /* Create ANVIL context */
    cg->anvil_ctx = anvil_ctx_create();
    if (!cg->anvil_ctx) {
        mcc_fatal(ctx, "Failed to create ANVIL context");
        return NULL;
    }
    
    return cg;
}

void mcc_codegen_destroy(mcc_codegen_t *cg)
{
    if (cg && cg->anvil_ctx) {
        anvil_ctx_destroy(cg->anvil_ctx);
    }
}

/* ============================================================
 * Public API - Configuration
 * ============================================================ */

void mcc_codegen_set_target(mcc_codegen_t *cg, mcc_arch_t arch)
{
    anvil_ctx_set_target(cg->anvil_ctx, mcc_arch_to_anvil(arch));
    
    /* Set Darwin ABI for macOS ARM64 */
    if (codegen_arch_is_darwin(arch)) {
        anvil_ctx_set_abi(cg->anvil_ctx, ANVIL_ABI_DARWIN);
    }
}

void mcc_codegen_set_opt_level(mcc_codegen_t *cg, mcc_opt_level_t level)
{
    /* Optimization level - stored for later use */
    (void)cg;
    (void)level;
}

/* ============================================================
 * Local Variable Management
 * ============================================================ */

anvil_value_t *codegen_find_local(mcc_codegen_t *cg, const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < cg->num_locals; i++) {
        if (cg->locals[i].name && strcmp(cg->locals[i].name, name) == 0) {
            return cg->locals[i].value;
        }
    }
    return NULL;
}

void codegen_add_local(mcc_codegen_t *cg, const char *name, anvil_value_t *value)
{
    if (cg->num_locals >= cg->cap_locals) {
        size_t new_cap = cg->cap_locals ? cg->cap_locals * 2 : 16;
        cg->locals = mcc_realloc(cg->mcc_ctx, cg->locals,
                                  cg->cap_locals * sizeof(cg->locals[0]),
                                  new_cap * sizeof(cg->locals[0]));
        cg->cap_locals = new_cap;
    }
    cg->locals[cg->num_locals].name = name;
    cg->locals[cg->num_locals].value = value;
    cg->num_locals++;
}

/* ============================================================
 * Global Variable Management
 * ============================================================ */

anvil_value_t *codegen_find_global(mcc_codegen_t *cg, const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < cg->num_globals; i++) {
        if (cg->globals[i].name && strcmp(cg->globals[i].name, name) == 0) {
            return cg->globals[i].value;
        }
    }
    return NULL;
}

anvil_value_t *codegen_get_or_add_global(mcc_codegen_t *cg, const char *name, anvil_type_t *type)
{
    /* Check if already exists */
    anvil_value_t *existing = codegen_find_global(cg, name);
    if (existing) return existing;
    
    /* Create new global */
    anvil_value_t *global = anvil_module_add_global(cg->anvil_mod, name, type, ANVIL_LINK_EXTERNAL);
    
    /* Add to cache */
    if (cg->num_globals >= cg->cap_globals) {
        size_t new_cap = cg->cap_globals ? cg->cap_globals * 2 : 8;
        cg->globals = mcc_realloc(cg->mcc_ctx, cg->globals,
                                   cg->cap_globals * sizeof(cg->globals[0]),
                                   new_cap * sizeof(cg->globals[0]));
        cg->cap_globals = new_cap;
    }
    cg->globals[cg->num_globals].name = name;
    cg->globals[cg->num_globals].value = global;
    cg->num_globals++;
    
    return global;
}

/* ============================================================
 * String Literal Management
 * ============================================================ */

anvil_value_t *codegen_get_string_literal(mcc_codegen_t *cg, const char *str)
{
    /* Check if already exists */
    for (size_t i = 0; i < cg->num_strings; i++) {
        if (strcmp(cg->strings[i].str, str) == 0) {
            return cg->strings[i].value;
        }
    }
    
    /* Create string constant */
    anvil_value_t *strval = anvil_const_string(cg->anvil_ctx, str);
    
    /* Add to pool */
    if (cg->num_strings >= cg->cap_strings) {
        size_t new_cap = cg->cap_strings ? cg->cap_strings * 2 : 8;
        cg->strings = mcc_realloc(cg->mcc_ctx, cg->strings,
                                   cg->cap_strings * sizeof(cg->strings[0]),
                                   new_cap * sizeof(cg->strings[0]));
        cg->cap_strings = new_cap;
    }
    cg->strings[cg->num_strings].str = str;
    cg->strings[cg->num_strings].value = strval;
    cg->num_strings++;
    
    return strval;
}

/* ============================================================
 * Label Management
 * ============================================================ */

anvil_block_t *codegen_get_label_block(mcc_codegen_t *cg, const char *name)
{
    for (size_t i = 0; i < cg->num_labels; i++) {
        if (strcmp(cg->labels[i].name, name) == 0) {
            return cg->labels[i].block;
        }
    }
    
    /* Create new block */
    anvil_block_t *block = anvil_block_create(cg->current_func, name);
    
    if (cg->num_labels >= cg->cap_labels) {
        size_t new_cap = cg->cap_labels ? cg->cap_labels * 2 : 8;
        cg->labels = mcc_realloc(cg->mcc_ctx, cg->labels,
                                  cg->cap_labels * sizeof(cg->labels[0]),
                                  new_cap * sizeof(cg->labels[0]));
        cg->cap_labels = new_cap;
    }
    cg->labels[cg->num_labels].name = name;
    cg->labels[cg->num_labels].block = block;
    cg->num_labels++;
    
    return block;
}

/* ============================================================
 * Block Management
 * ============================================================ */

void codegen_set_current_block(mcc_codegen_t *cg, anvil_block_t *block)
{
    cg->current_block = block;
    anvil_set_insert_point(cg->anvil_ctx, block);
}

bool codegen_block_has_terminator(mcc_codegen_t *cg)
{
    if (!cg || !cg->current_block) return false;
    return anvil_block_has_terminator(cg->current_block);
}

/* ============================================================
 * Function Management
 * ============================================================ */

anvil_func_t *codegen_find_func(mcc_codegen_t *cg, mcc_symbol_t *sym)
{
    for (size_t i = 0; i < cg->num_funcs; i++) {
        if (cg->funcs[i].sym == sym) {
            return cg->funcs[i].func;
        }
    }
    return NULL;
}

void codegen_add_func(mcc_codegen_t *cg, mcc_symbol_t *sym, anvil_func_t *func)
{
    if (cg->num_funcs >= cg->cap_funcs) {
        size_t new_cap = cg->cap_funcs ? cg->cap_funcs * 2 : 16;
        cg->funcs = mcc_realloc(cg->mcc_ctx, cg->funcs,
                                 cg->cap_funcs * sizeof(cg->funcs[0]),
                                 new_cap * sizeof(cg->funcs[0]));
        cg->cap_funcs = new_cap;
    }
    cg->funcs[cg->num_funcs].sym = sym;
    cg->funcs[cg->num_funcs].func = func;
    cg->num_funcs++;
}

anvil_func_t *codegen_get_or_declare_func(mcc_codegen_t *cg, mcc_symbol_t *sym)
{
    /* Check if already declared */
    anvil_func_t *func = codegen_find_func(cg, sym);
    if (func) return func;
    
    /* Create function type */
    anvil_type_t *func_type = codegen_type(cg, sym->type);
    
    /* Declare the function */
    func = anvil_func_declare(cg->anvil_mod, sym->name, func_type);
    codegen_add_func(cg, sym, func);
    
    return func;
}

/* ============================================================
 * Public API - Code Generation
 * ============================================================ */

bool mcc_codegen_generate(mcc_codegen_t *cg, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    /* Create module */
    cg->anvil_mod = anvil_module_create(cg->anvil_ctx, "mcc_output");
    
    /* Generate code for all declarations */
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        codegen_decl(cg, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(cg->mcc_ctx);
}

bool mcc_codegen_add_ast(mcc_codegen_t *cg, mcc_ast_node_t *ast)
{
    if (!ast || ast->kind != AST_TRANSLATION_UNIT) {
        return false;
    }
    
    /* Create module if not already created */
    if (!cg->anvil_mod) {
        cg->anvil_mod = anvil_module_create(cg->anvil_ctx, "mcc_output");
    }
    
    /* Generate code for all declarations in this AST */
    for (size_t i = 0; i < ast->data.translation_unit.num_decls; i++) {
        codegen_decl(cg, ast->data.translation_unit.decls[i]);
    }
    
    return !mcc_has_errors(cg->mcc_ctx);
}

bool mcc_codegen_finalize(mcc_codegen_t *cg)
{
    /* Currently nothing special to do - module is ready for codegen */
    /* Future: could add link-time optimizations, symbol resolution checks, etc. */
    return !mcc_has_errors(cg->mcc_ctx);
}

char *mcc_codegen_get_output(mcc_codegen_t *cg, size_t *len)
{
    if (!cg->anvil_mod) {
        *len = 0;
        return NULL;
    }
    
    /* Generate code */
    char *output = NULL;
    anvil_error_t err = anvil_module_codegen(cg->anvil_mod, &output, len);
    if (err != ANVIL_OK) {
        *len = 0;
        return NULL;
    }
    
    return output;
}

/* ============================================================
 * Public API Wrappers (for backward compatibility)
 * ============================================================ */

anvil_type_t *mcc_codegen_type(mcc_codegen_t *cg, mcc_type_t *type)
{
    return codegen_type(cg, type);
}

anvil_value_t *mcc_codegen_expr(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    return codegen_expr(cg, expr);
}

anvil_value_t *mcc_codegen_lvalue(mcc_codegen_t *cg, mcc_ast_node_t *expr)
{
    return codegen_lvalue(cg, expr);
}

void mcc_codegen_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_stmt(cg, stmt);
}

void mcc_codegen_compound_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_compound_stmt(cg, stmt);
}

void mcc_codegen_if_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_if_stmt(cg, stmt);
}

void mcc_codegen_while_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_while_stmt(cg, stmt);
}

void mcc_codegen_do_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_do_stmt(cg, stmt);
}

void mcc_codegen_for_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_for_stmt(cg, stmt);
}

void mcc_codegen_switch_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_switch_stmt(cg, stmt);
}

void mcc_codegen_return_stmt(mcc_codegen_t *cg, mcc_ast_node_t *stmt)
{
    codegen_return_stmt(cg, stmt);
}

void mcc_codegen_func(mcc_codegen_t *cg, mcc_ast_node_t *func)
{
    codegen_func(cg, func);
}

void mcc_codegen_global_var(mcc_codegen_t *cg, mcc_ast_node_t *var)
{
    codegen_global_var(cg, var);
}

void mcc_codegen_decl(mcc_codegen_t *cg, mcc_ast_node_t *decl)
{
    codegen_decl(cg, decl);
}
