/*
 * ANVIL - Module implementation
 */

#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>

anvil_module_t *anvil_module_create(anvil_ctx_t *ctx, const char *name)
{
    if (!ctx) return NULL;
    
    anvil_module_t *mod = calloc(1, sizeof(anvil_module_t));
    if (!mod) return NULL;
    
    mod->name = name ? strdup(name) : strdup("module");
    mod->ctx = ctx;
    
    /* Add to context's module list */
    mod->next = ctx->modules;
    ctx->modules = mod;
    
    return mod;
}

void anvil_module_destroy(anvil_module_t *mod)
{
    if (!mod) return;
    
    /* Remove from context's module list to prevent double-free */
    if (mod->ctx) {
        anvil_module_t **pp = &mod->ctx->modules;
        while (*pp) {
            if (*pp == mod) {
                *pp = mod->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    
    /* Destroy functions */
    anvil_func_t *func = mod->funcs;
    while (func) {
        anvil_func_t *next = func->next;
        
        /* Destroy blocks */
        anvil_block_t *block = func->blocks;
        while (block) {
            anvil_block_t *bnext = block->next;
            
            /* Destroy instructions */
            anvil_instr_t *instr = block->first;
            while (instr) {
                anvil_instr_t *inext = instr->next;
                
                /* Free operands that are constants (owned by instruction) 
                 * Note: ANVIL_VAL_FUNC values are owned by the function, not the instruction */
                for (size_t i = 0; i < instr->num_operands; i++) {
                    anvil_value_t *op = instr->operands[i];
                    if (op && (op->kind == ANVIL_VAL_CONST_INT || 
                               op->kind == ANVIL_VAL_CONST_FLOAT ||
                               op->kind == ANVIL_VAL_CONST_NULL ||
                               op->kind == ANVIL_VAL_CONST_STRING)) {
                        if (op->kind == ANVIL_VAL_CONST_STRING && op->data.str) {
                            free((void*)op->data.str);
                            op->data.str = NULL;  /* Prevent double-free */
                        }
                        free(op->name);
                        op->name = NULL;  /* Prevent double-free */
                        free(op);
                        instr->operands[i] = NULL;  /* Mark as freed */
                    }
                    /* ANVIL_VAL_FUNC values are freed with their function, not here */
                }
                free(instr->operands);
                free(instr->phi_blocks);
                if (instr->result) {
                    free(instr->result->name);
                    free(instr->result);
                }
                free(instr);
                instr = inext;
            }
            
            free(block->name);
            free(block->preds);
            free(block->succs);
            free(block);
            block = bnext;
        }
        
        /* Destroy params */
        for (size_t i = 0; i < func->num_params; i++) {
            if (func->params[i]) {
                free(func->params[i]->name);
                free(func->params[i]);
            }
        }
        free(func->params);
        
        /* Destroy function value */
        if (func->value) {
            free(func->value->name);
            free(func->value);
        }
        
        free(func->name);
        free(func);
        func = next;
    }
    
    /* Destroy globals */
    anvil_global_t *global = mod->globals;
    while (global) {
        anvil_global_t *next = global->next;
        if (global->value) {
            free(global->value->name);
            free(global->value);
        }
        free(global);
        global = next;
    }
    
    /* Destroy string table */
    free(mod->strings.strings);
    
    free(mod->name);
    free(mod);
}

anvil_value_t *anvil_module_add_global(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type, anvil_linkage_t linkage)
{
    if (!mod || !type) return NULL;
    
    anvil_global_t *global = calloc(1, sizeof(anvil_global_t));
    if (!global) return NULL;
    
    anvil_value_t *val = anvil_value_create(mod->ctx, ANVIL_VAL_GLOBAL, type, name);
    if (!val) {
        free(global);
        return NULL;
    }
    
    val->data.global.linkage = linkage;
    val->data.global.init = NULL;
    
    global->value = val;
    global->next = mod->globals;
    mod->globals = global;
    mod->num_globals++;
    
    return val;
}

anvil_value_t *anvil_module_add_extern(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type)
{
    return anvil_module_add_global(mod, name, type, ANVIL_LINK_EXTERNAL);
}

anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len)
{
    if (!mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    anvil_ctx_t *ctx = mod->ctx;
    if (!ctx->backend) {
        anvil_set_error(ctx, ANVIL_ERR_NO_BACKEND, "No backend configured");
        return ANVIL_ERR_NO_BACKEND;
    }
    
    return ctx->backend->ops->codegen_module(ctx->backend, mod, output, len);
}

anvil_error_t anvil_module_write(anvil_module_t *mod, const char *filename)
{
    if (!mod || !filename) return ANVIL_ERR_INVALID_ARG;
    
    char *output = NULL;
    size_t len = 0;
    
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    if (err != ANVIL_OK) return err;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        free(output);
        anvil_set_error(mod->ctx, ANVIL_ERR_IO, "Cannot open file: %s", filename);
        return ANVIL_ERR_IO;
    }
    
    fwrite(output, 1, len, f);
    fclose(f);
    free(output);
    
    return ANVIL_OK;
}
