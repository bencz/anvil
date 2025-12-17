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
    
    /* First pass: collect ALL unique constants from ALL functions */
    #define MAX_CONSTS 4096
    anvil_value_t *all_consts[MAX_CONSTS];
    size_t num_all_consts = 0;
    
    for (anvil_func_t *f = mod->funcs; f; f = f->next) {
        for (anvil_block_t *b = f->blocks; b; b = b->next) {
            for (anvil_instr_t *instr = b->first; instr; instr = instr->next) {
                for (size_t i = 0; i < instr->num_operands; i++) {
                    anvil_value_t *op = instr->operands[i];
                    if (op && (op->kind == ANVIL_VAL_CONST_INT || 
                               op->kind == ANVIL_VAL_CONST_FLOAT ||
                               op->kind == ANVIL_VAL_CONST_NULL ||
                               op->kind == ANVIL_VAL_CONST_STRING ||
                               op->kind == ANVIL_VAL_CONST_ARRAY)) {
                        /* Check if already in list */
                        bool found = false;
                        for (size_t j = 0; j < num_all_consts; j++) {
                            if (all_consts[j] == op) { found = true; break; }
                        }
                        if (!found && num_all_consts < MAX_CONSTS) {
                            all_consts[num_all_consts++] = op;
                        }
                    }
                    instr->operands[i] = NULL;
                }
            }
        }
    }
    
    /* Free all collected constants */
    for (size_t i = 0; i < num_all_consts; i++) {
        anvil_value_t *op = all_consts[i];
        if (op->kind == ANVIL_VAL_CONST_STRING && op->data.str) {
            free((void*)op->data.str);
        } else if (op->kind == ANVIL_VAL_CONST_ARRAY && op->data.array.elements) {
            free(op->data.array.elements);
        }
        free(op->name);
        free(op);
    }
    #undef MAX_CONSTS
    
    /* Destroy functions */
    anvil_func_t *func = mod->funcs;
    while (func) {
        anvil_func_t *next = func->next;
        
        anvil_block_t *block = func->blocks;
        
        /* Free all instruction results */
        for (anvil_block_t *b = block; b; b = b->next) {
            for (anvil_instr_t *instr = b->first; instr; instr = instr->next) {
                if (instr->result) {
                    free(instr->result->name);
                    free(instr->result);
                    instr->result = NULL;
                }
            }
        }
        
        /* Free instructions and blocks */
        while (block) {
            anvil_block_t *bnext = block->next;
            
            anvil_instr_t *instr = block->first;
            while (instr) {
                anvil_instr_t *inext = instr->next;
                free(instr->operands);
                free(instr->phi_blocks);
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
        if (func->params) {
            for (size_t i = 0; i < func->num_params; i++) {
                if (func->params[i]) {
                    free(func->params[i]->name);
                    free(func->params[i]);
                }
            }
            free(func->params);
        }
        
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
    
    /* Call prepare_ir if the backend provides it */
    if (ctx->backend->ops->prepare_ir) {
        anvil_error_t err = ctx->backend->ops->prepare_ir(ctx->backend, mod);
        if (err != ANVIL_OK) return err;
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
