/*
 * ANVIL - Optimization Pass Manager Implementation
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Pass manager structure */
struct anvil_pass_manager {
    anvil_ctx_t *ctx;
    anvil_opt_level_t level;
    bool enabled[ANVIL_PASS_COUNT];
    
    /* Custom passes */
    anvil_pass_info_t *custom_passes;
    size_t num_custom;
    size_t cap_custom;
};

/* Built-in pass definitions */
static const anvil_pass_info_t builtin_passes[ANVIL_PASS_COUNT] = {
    {
        .id = ANVIL_PASS_CONST_FOLD,
        .name = "const-fold",
        .description = "Constant folding",
        .run = anvil_pass_const_fold,
        .min_level = ANVIL_OPT_BASIC
    },
    {
        .id = ANVIL_PASS_DCE,
        .name = "dce",
        .description = "Dead code elimination",
        .run = anvil_pass_dce,
        .min_level = ANVIL_OPT_BASIC
    },
    {
        .id = ANVIL_PASS_SIMPLIFY_CFG,
        .name = "simplify-cfg",
        .description = "Simplify control flow graph",
        .run = anvil_pass_simplify_cfg,
        .min_level = ANVIL_OPT_STANDARD
    },
    {
        .id = ANVIL_PASS_STRENGTH_REDUCE,
        .name = "strength-reduce",
        .description = "Strength reduction",
        .run = anvil_pass_strength_reduce,
        .min_level = ANVIL_OPT_STANDARD
    },
    {
        .id = ANVIL_PASS_COPY_PROP,
        .name = "copy-prop",
        .description = "Copy propagation",
        .run = anvil_pass_copy_prop,
        .min_level = ANVIL_OPT_BASIC
    },
    {
        .id = ANVIL_PASS_DEAD_STORE,
        .name = "dead-store",
        .description = "Dead store elimination",
        .run = anvil_pass_dead_store,
        .min_level = ANVIL_OPT_STANDARD
    },
    {
        .id = ANVIL_PASS_LOAD_ELIM,
        .name = "load-elim",
        .description = "Redundant load elimination",
        .run = anvil_pass_load_elim,
        .min_level = ANVIL_OPT_STANDARD
    },
    {
        .id = ANVIL_PASS_LOOP_UNROLL,
        .name = "loop-unroll",
        .description = "Loop unrolling",
        .run = NULL,  /* Disabled - needs more testing */
        .min_level = ANVIL_OPT_AGGRESSIVE
    },
    {
        .id = ANVIL_PASS_COMMON_SUBEXPR,
        .name = "cse",
        .description = "Common subexpression elimination",
        .run = anvil_pass_cse,
        .min_level = ANVIL_OPT_STANDARD
    }
};

/* ============================================================================
 * Pass Manager Implementation
 * ============================================================================ */

anvil_pass_manager_t *anvil_pass_manager_create(anvil_ctx_t *ctx)
{
    if (!ctx) return NULL;
    
    anvil_pass_manager_t *pm = calloc(1, sizeof(anvil_pass_manager_t));
    if (!pm) return NULL;
    
    pm->ctx = ctx;
    pm->level = ANVIL_OPT_NONE;
    
    /* All passes disabled by default */
    for (int i = 0; i < ANVIL_PASS_COUNT; i++) {
        pm->enabled[i] = false;
    }
    
    return pm;
}

void anvil_pass_manager_destroy(anvil_pass_manager_t *pm)
{
    if (!pm) return;
    free(pm->custom_passes);
    free(pm);
}

void anvil_pass_manager_set_level(anvil_pass_manager_t *pm, anvil_opt_level_t level)
{
    if (!pm) return;
    
    pm->level = level;
    
    /* Enable/disable passes based on level */
    for (int i = 0; i < ANVIL_PASS_COUNT; i++) {
        pm->enabled[i] = (level >= builtin_passes[i].min_level);
    }
}

anvil_opt_level_t anvil_pass_manager_get_level(anvil_pass_manager_t *pm)
{
    return pm ? pm->level : ANVIL_OPT_NONE;
}

void anvil_pass_manager_enable(anvil_pass_manager_t *pm, anvil_pass_id_t pass)
{
    if (!pm || pass >= ANVIL_PASS_COUNT) return;
    pm->enabled[pass] = true;
}

void anvil_pass_manager_disable(anvil_pass_manager_t *pm, anvil_pass_id_t pass)
{
    if (!pm || pass >= ANVIL_PASS_COUNT) return;
    pm->enabled[pass] = false;
}

bool anvil_pass_manager_is_enabled(anvil_pass_manager_t *pm, anvil_pass_id_t pass)
{
    if (!pm || pass >= ANVIL_PASS_COUNT) return false;
    return pm->enabled[pass];
}

bool anvil_pass_manager_run_func(anvil_pass_manager_t *pm, anvil_func_t *func)
{
    if (!pm || !func) return false;
    if (func->is_declaration) return false;  /* Skip declarations */
    
    bool changed = false;
    bool any_changed;
    int iterations = 0;
    const int max_iterations = 10;  /* Prevent infinite loops */
    
    /* Run passes until fixpoint or max iterations */
    do {
        any_changed = false;
        
        /* Run built-in passes */
        for (int i = 0; i < ANVIL_PASS_COUNT; i++) {
            if (pm->enabled[i] && builtin_passes[i].run) {
                if (builtin_passes[i].run(func)) {
                    any_changed = true;
                    changed = true;
                }
            }
        }
        
        /* Run custom passes */
        for (size_t i = 0; i < pm->num_custom; i++) {
            if (pm->custom_passes[i].run) {
                if (pm->custom_passes[i].run(func)) {
                    any_changed = true;
                    changed = true;
                }
            }
        }
        
        iterations++;
    } while (any_changed && iterations < max_iterations);
    
    return changed;
}

bool anvil_pass_manager_run_module(anvil_pass_manager_t *pm, anvil_module_t *mod)
{
    if (!pm || !mod) return false;
    
    bool changed = false;
    
    /* Run passes on each function */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (anvil_pass_manager_run_func(pm, func)) {
            changed = true;
        }
    }
    
    return changed;
}

anvil_error_t anvil_pass_manager_register(anvil_pass_manager_t *pm,
                                           const anvil_pass_info_t *pass)
{
    if (!pm || !pass) return ANVIL_ERR_INVALID_ARG;
    
    /* Grow array if needed */
    if (pm->num_custom >= pm->cap_custom) {
        size_t new_cap = pm->cap_custom ? pm->cap_custom * 2 : 4;
        anvil_pass_info_t *new_passes = realloc(pm->custom_passes,
                                                 new_cap * sizeof(anvil_pass_info_t));
        if (!new_passes) return ANVIL_ERR_NOMEM;
        pm->custom_passes = new_passes;
        pm->cap_custom = new_cap;
    }
    
    pm->custom_passes[pm->num_custom++] = *pass;
    return ANVIL_OK;
}
