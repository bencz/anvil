/*
 * ANVIL - Function and basic block implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

static void free_unlinked_func(anvil_func_t *func)
{
    if (!func) return;
    free(func->params);
    free(func->name);
    free(func);
}

static void register_owned_func(anvil_ctx_t *ctx, anvil_func_t *func)
{
    func->owner_ctx = ctx;
    func->ctx_next_owned = ctx->owned_funcs;
    ctx->owned_funcs = func;
}

void anvil_func_free_all(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    anvil_func_t *func = ctx->owned_funcs;
    ctx->owned_funcs = NULL;
    while (func) {
        anvil_func_t *next = func->ctx_next_owned;
        free(func->params);
        free(func->name);
        free(func);
        func = next;
    }
}

static bool materialize_decl_body(anvil_func_t *func,
                                  anvil_linkage_t linkage)
{
    if (!func || !func->is_declaration || !func->parent) return false;
    anvil_ctx_t *ctx = func->parent->ctx;
    size_t num_params = func->type->data.func.num_params;
    anvil_value_t **params = NULL;
    if (num_params) {
        if (num_params > SIZE_MAX / sizeof(*params)) {
            anvil_set_error(ctx, ANVIL_ERR_NOMEM,
                            "Function parameter array overflows size_t");
            return false;
        }
        params = anvil_ctx_calloc(ctx, num_params, sizeof(*params));
        if (!params) return false;
        for (size_t i = 0; i < num_params; i++) {
            char param_name[32];
            snprintf(param_name, sizeof(param_name), "arg%zu", i);
            params[i] = anvil_value_create(ctx, ANVIL_VAL_PARAM,
                                            func->type->data.func.params[i],
                                            param_name);
            if (!params[i]) { free(params); return false; }
            params[i]->data.param.index = i;
            params[i]->data.param.func = func;
        }
    }

    anvil_block_t *entry = anvil_block_create(func, "entry");
    if (!entry) { free(params); return false; }
    for (size_t i = 0; i < num_params; i++)
        params[i]->owner_module = func->parent;
    func->params = params;
    func->num_params = num_params;
    func->entry = entry;
    func->linkage = linkage;
    func->is_declaration = false;
    return true;
}

anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage)
{
    if (!mod) return NULL;
    if (!name || !*name || !type ||
        (unsigned)linkage > (unsigned)ANVIL_LINK_WEAK) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_ARG,
                        "Function definition has invalid arguments");
        return NULL;
    }
    if (type->owner_ctx != mod->ctx || type->kind != ANVIL_TYPE_FUNC) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                        "Function definition requires a function type from its module context");
        return NULL;
    }
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!anvil_cc_resolve(mod->ctx, type->data.func.cc, &effective_cc) ||
        effective_cc != type->data.func.cc) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                        "Function definition has a non-canonical or incompatible calling convention");
        return NULL;
    }
    anvil_value_t *existing_value = anvil_module_lookup_symbol(mod, name);
    if (existing_value) {
        if (existing_value->kind != ANVIL_VAL_FUNC ||
            !existing_value->data.func ||
            !anvil_types_equal(existing_value->data.func->type, type)) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                            "Symbol '%s' redeclared with a different kind or type",
                            name);
            return NULL;
        }
        anvil_func_t *existing = existing_value->data.func;
        if (!anvil_symbol_linkage_compatible(existing->linkage,
                                              existing->is_declaration,
                                              linkage, false, true)) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                            "Function '%s' redeclared with incompatible linkage", name);
            return NULL;
        }
        if (!existing->is_declaration) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                            "Function '%s' is already defined", name);
            return NULL;
        }
        return materialize_decl_body(existing, linkage) ? existing : NULL;
    }
    if (mod->ctx->next_func_id == UINT32_MAX) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                        "IR function ID space is exhausted");
        return NULL;
    }
    if (!anvil_module_symbol_prepare(mod, 1)) {
        anvil_set_error(mod->ctx, ANVIL_ERR_NOMEM,
                        "Out of memory growing module symbol table");
        return NULL;
    }
    
    anvil_func_t *func = anvil_ctx_calloc(mod->ctx, 1, sizeof(*func));
    if (!func) return NULL;
    
    func->name = anvil_ctx_strdup(mod->ctx, name);
    if (!func->name) {
        free(func);
        return NULL;
    }
    func->type = type;
    func->linkage = linkage;
    func->parent = mod;
    func->id = mod->ctx->next_func_id++;
    func->is_declaration = false;
    
    /* Create parameters */
    size_t num_params = type->data.func.num_params;
    func->num_params = num_params;
    
    if (num_params > 0) {
        func->params = anvil_ctx_calloc(mod->ctx, num_params,
                                        sizeof(*func->params));
        if (!func->params) {
            free(func->name);
            free(func);
            return NULL;
        }
        
        for (size_t i = 0; i < num_params; i++) {
            char param_name[32];
            snprintf(param_name, sizeof(param_name), "arg%zu", i);
            
            anvil_value_t *param = anvil_value_create(mod->ctx, ANVIL_VAL_PARAM,
                                                       type->data.func.params[i], param_name);
            if (!param) {
                /* Cleanup on failure */
                free_unlinked_func(func);
                return NULL;
            }
            
            param->data.param.index = i;
            param->data.param.func = func;
            param->owner_module = mod;
            func->params[i] = param;
        }
    }
    
    /* Create entry block */
    func->entry = anvil_block_create(func, "entry");
    if (!func->entry) {
        free_unlinked_func(func);
        return NULL;
    }
    
    /* Function values are callable addresses, so their SSA type is ptr<func>. */
    anvil_type_t *func_ptr_type = anvil_type_ptr(mod->ctx, type);
    if (!func_ptr_type) {
        free_unlinked_func(func);
        return NULL;
    }
    func->value = anvil_value_create(mod->ctx, ANVIL_VAL_FUNC, func_ptr_type, name);
    if (!func->value) {
        free_unlinked_func(func);
        return NULL;
    }
    func->value->data.func = func;
    func->value->owner_module = mod;
    
    /* Add to module's function list */
    func->next = mod->funcs;
    mod->funcs = func;
    mod->num_funcs++;
    anvil_module_symbol_register(mod, func->value);
    register_owned_func(mod->ctx, func);
    
    return func;
}

anvil_func_t *anvil_func_declare_linkage(anvil_module_t *mod,
                                          const char *name,
                                          anvil_type_t *type,
                                          anvil_linkage_t linkage)
{
    if (!mod) return NULL;
    if (!name || !*name || !type ||
        (unsigned)linkage > (unsigned)ANVIL_LINK_WEAK) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_ARG,
                        "Function declaration has invalid arguments");
        return NULL;
    }
    if (type->owner_ctx != mod->ctx || type->kind != ANVIL_TYPE_FUNC) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                        "Function declaration requires a function type from its module context");
        return NULL;
    }
    anvil_cc_t effective_cc = ANVIL_CC_DEFAULT;
    if (!anvil_cc_resolve(mod->ctx, type->data.func.cc, &effective_cc) ||
        effective_cc != type->data.func.cc) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                        "Function declaration has a non-canonical or incompatible calling convention");
        return NULL;
    }
    anvil_value_t *existing_value = anvil_module_lookup_symbol(mod, name);
    if (existing_value) {
        if (existing_value->kind != ANVIL_VAL_FUNC ||
            !existing_value->data.func ||
            !anvil_types_equal(existing_value->data.func->type, type) ||
            !anvil_symbol_linkage_compatible(
                existing_value->data.func->linkage,
                existing_value->data.func->is_declaration,
                linkage, true, true)) {
            anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_TYPE,
                            "Function declaration '%s' conflicts with an existing symbol",
                            name);
            return NULL;
        }
        return existing_value->data.func;
    }
    if (mod->ctx->next_func_id == UINT32_MAX) {
        anvil_set_error(mod->ctx, ANVIL_ERR_INVALID_OP,
                        "IR function ID space is exhausted");
        return NULL;
    }
    if (!anvil_module_symbol_prepare(mod, 1)) {
        anvil_set_error(mod->ctx, ANVIL_ERR_NOMEM,
                        "Out of memory growing module symbol table");
        return NULL;
    }
    
    anvil_func_t *func = anvil_ctx_calloc(mod->ctx, 1, sizeof(*func));
    if (!func) return NULL;
    
    func->name = anvil_ctx_strdup(mod->ctx, name);
    if (!func->name) {
        free(func);
        return NULL;
    }
    func->type = type;
    func->linkage = linkage;
    func->parent = mod;
    func->id = mod->ctx->next_func_id++;
    func->is_declaration = true;
    
    /* No parameters, entry block, or body for declarations */
    func->num_params = type->data.func.num_params;
    func->params = NULL;
    func->entry = NULL;
    func->blocks = NULL;
    
    /* Function values are callable addresses, so their SSA type is ptr<func>. */
    anvil_type_t *func_ptr_type = anvil_type_ptr(mod->ctx, type);
    if (!func_ptr_type) {
        free_unlinked_func(func);
        return NULL;
    }
    func->value = anvil_value_create(mod->ctx, ANVIL_VAL_FUNC, func_ptr_type, name);
    if (!func->value) {
        free_unlinked_func(func);
        return NULL;
    }
    func->value->data.func = func;
    func->value->owner_module = mod;
    
    /* Add to module's function list */
    func->next = mod->funcs;
    mod->funcs = func;
    mod->num_funcs++;
    anvil_module_symbol_register(mod, func->value);
    register_owned_func(mod->ctx, func);
    
    return func;
}

anvil_func_t *anvil_func_declare(anvil_module_t *mod, const char *name,
                                  anvil_type_t *type)
{
    return anvil_func_declare_linkage(mod, name, type, ANVIL_LINK_EXTERNAL);
}

anvil_value_t *anvil_func_get_value(anvil_func_t *func)
{
    return func ? func->value : NULL;
}

anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index)
{
    if (!func || !func->params || index >= func->num_params) return NULL;
    return func->params[index];
}

anvil_block_t *anvil_func_get_entry(anvil_func_t *func)
{
    return func ? func->entry : NULL;
}

anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name)
{
    if (!func) return NULL;
    anvil_ctx_t *ctx = func->parent ? func->parent->ctx : NULL;
    if (!ctx) return NULL;
    if (ctx->next_block_id == UINT32_MAX) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_OP,
                        "IR basic-block ID space is exhausted");
        return NULL;
    }
    
    anvil_block_t *block = anvil_ctx_calloc(ctx, 1, sizeof(*block));
    if (!block) return NULL;
    
    block->name = name ? anvil_ctx_strdup(ctx, name) : NULL;
    if (name && !block->name) {
        free(block);
        return NULL;
    }
    block->parent = func;
    block->owner_module = func->parent;
    block->id = func->parent->ctx->next_block_id++;
    block->ctx_next_owned = func->parent->ctx->owned_blocks;
    func->parent->ctx->owned_blocks = block;
    
    /* Append to function's block list in O(1) using the cached tail. */
    if (!func->blocks) {
        func->blocks = block;
    } else {
        func->last_block->next = block;
    }
    func->last_block = block;
    func->num_blocks++;

    return block;
}

const char *anvil_block_get_name(anvil_block_t *block)
{
    return block ? block->name : NULL;
}

bool anvil_block_has_terminator(anvil_block_t *block)
{
    if (!block || !block->last) return false;
    
    anvil_op_t op = block->last->op;
    return op == ANVIL_OP_RET ||
           op == ANVIL_OP_BR ||
           op == ANVIL_OP_BR_COND ||
           op == ANVIL_OP_SWITCH;
}
