/*
 * ANVIL - Function and basic block implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage)
{
    if (!mod || !name || !type) return NULL;
    if (type->kind != ANVIL_TYPE_FUNC) return NULL;
    
    anvil_func_t *func = calloc(1, sizeof(anvil_func_t));
    if (!func) return NULL;
    
    func->name = strdup(name);
    func->type = type;
    func->linkage = linkage;
    func->cc = ANVIL_CC_DEFAULT;
    func->parent = mod;
    func->id = mod->ctx->next_func_id++;
    func->is_declaration = false;
    
    /* Create parameters */
    size_t num_params = type->data.func.num_params;
    func->num_params = num_params;
    
    if (num_params > 0) {
        func->params = calloc(num_params, sizeof(anvil_value_t *));
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
                for (size_t j = 0; j < i; j++) {
                    free(func->params[j]->name);
                    free(func->params[j]);
                }
                free(func->params);
                free(func->name);
                free(func);
                return NULL;
            }
            
            param->data.param.index = i;
            param->data.param.func = func;
            func->params[i] = param;
        }
    }
    
    /* Create entry block */
    func->entry = anvil_block_create(func, "entry");
    
    /* Create value for function (for use in calls) */
    func->value = anvil_value_create(mod->ctx, ANVIL_VAL_FUNC, type, name);
    if (func->value) {
        func->value->data.func = func;
    }
    
    /* Add to module's function list */
    func->next = mod->funcs;
    mod->funcs = func;
    mod->num_funcs++;
    
    return func;
}

anvil_func_t *anvil_func_declare(anvil_module_t *mod, const char *name,
                                  anvil_type_t *type)
{
    if (!mod || !name || !type) return NULL;
    if (type->kind != ANVIL_TYPE_FUNC) return NULL;
    
    anvil_func_t *func = calloc(1, sizeof(anvil_func_t));
    if (!func) return NULL;
    
    func->name = strdup(name);
    func->type = type;
    func->linkage = ANVIL_LINK_EXTERNAL;
    func->cc = ANVIL_CC_DEFAULT;
    func->parent = mod;
    func->id = mod->ctx->next_func_id++;
    func->is_declaration = true;
    
    /* No parameters, entry block, or body for declarations */
    func->num_params = type->data.func.num_params;
    func->params = NULL;
    func->entry = NULL;
    func->blocks = NULL;
    
    /* Create value for function (for use in calls) */
    func->value = anvil_value_create(mod->ctx, ANVIL_VAL_FUNC, type, name);
    if (func->value) {
        func->value->data.func = func;
    }
    
    /* Add to module's function list */
    func->next = mod->funcs;
    mod->funcs = func;
    mod->num_funcs++;
    
    return func;
}

anvil_value_t *anvil_func_get_value(anvil_func_t *func)
{
    return func ? func->value : NULL;
}

void anvil_func_set_cc(anvil_func_t *func, anvil_cc_t cc)
{
    if (func) func->cc = cc;
}

anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index)
{
    if (!func || index >= func->num_params) return NULL;
    return func->params[index];
}

anvil_block_t *anvil_func_get_entry(anvil_func_t *func)
{
    return func ? func->entry : NULL;
}

anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name)
{
    if (!func) return NULL;
    
    anvil_block_t *block = calloc(1, sizeof(anvil_block_t));
    if (!block) return NULL;
    
    block->name = name ? strdup(name) : NULL;
    block->parent = func;
    block->id = func->parent->ctx->next_block_id++;
    
    /* Add to function's block list */
    if (!func->blocks) {
        func->blocks = block;
    } else {
        anvil_block_t *last = func->blocks;
        while (last->next) last = last->next;
        last->next = block;
    }
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
    return op == ANVIL_OP_RET || op == ANVIL_OP_BR || op == ANVIL_OP_BR_COND;
}
