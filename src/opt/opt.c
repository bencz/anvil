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
    unsigned iteration_limit;
    bool enabled[ANVIL_PASS_COUNT];

    /* Custom passes */
    anvil_pass_info_t *custom_passes;
    size_t num_custom;
    size_t cap_custom;
};

#define ANVIL_DEFAULT_PASS_ITERATION_LIMIT 10u

/* Execution order of the built-in passes. The order here is independent of
 * the enum values — run passes that expose opportunities first and let DCE
 * sweep up at the end of each fixpoint iteration. Without this explicit
 * ordering the passes ran alphabetically by enum, which meant strength
 * reduction ran before copy propagation and missed pow-of-2 simplifications,
 * and the dead-code sweep ran before the passes that create NOPs. */
static const int pass_exec_order[ANVIL_PASS_COUNT] = {
    ANVIL_PASS_COPY_PROP,          /* 1. Propagate copies — exposes constants */
    ANVIL_PASS_CONST_FOLD,         /* 2. Fold with freshly propagated constants */
    ANVIL_PASS_COMMON_SUBEXPR,     /* 3. CSE exposes further dead/copy patterns */
    ANVIL_PASS_STRENGTH_REDUCE,    /* 4. Strength reduction sees post-fold consts */
    ANVIL_PASS_STORE_LOAD_PROP,    /* 5. Memory passes — do them together */
    ANVIL_PASS_DEAD_STORE,
    ANVIL_PASS_LOAD_ELIM,
    ANVIL_PASS_SIMPLIFY_CFG,       /* 6. Clean up CFG after the rewrites */
    ANVIL_PASS_DCE,                /* 7. DCE mops up everything NOP'd above */
};

/* Built-in pass definitions
 * 
 * Optimization levels:
 *   O0 (NONE)       - No optimizations
 *   Og (DEBUG)      - Debug-friendly: copy_prop, store_load_prop (minimal IR cleanup)
 *   O1 (BASIC)      - Basic: const_fold, dce, copy_prop, store_load_prop
 *   O2 (STANDARD)   - Standard: O1 + simplify_cfg, strength_reduce, dead_store, load_elim, cse
 *   O3 (AGGRESSIVE) - Currently the same verified pass set as O2
 */
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
        .min_level = ANVIL_OPT_DEBUG  /* Og+ */
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
        .id = ANVIL_PASS_STORE_LOAD_PROP,
        .name = "store-load-prop",
        .description = "Store-load propagation",
        .run = anvil_pass_store_load_prop,
        .min_level = ANVIL_OPT_DEBUG  /* Og+ */
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
    
    anvil_pass_manager_t *pm = anvil_ctx_calloc(ctx, 1, sizeof(*pm));
    if (!pm) return NULL;
    
    pm->ctx = ctx;
    pm->level = ANVIL_OPT_NONE;
    pm->iteration_limit = ANVIL_DEFAULT_PASS_ITERATION_LIMIT;
    
    /* All passes disabled by default */
    for (int i = 0; i < ANVIL_PASS_COUNT; i++) {
        pm->enabled[i] = false;
    }
    
    return pm;
}

void anvil_pass_manager_destroy(anvil_pass_manager_t *pm)
{
    if (!pm) return;
    for (size_t i = 0; i < pm->num_custom; i++) {
        free((char *)pm->custom_passes[i].name);
        free((char *)pm->custom_passes[i].description);
    }
    free(pm->custom_passes);
    free(pm);
}

anvil_error_t anvil_pass_manager_set_level(anvil_pass_manager_t *pm,
                                            anvil_opt_level_t level)
{
    if (!pm) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)level > (unsigned)ANVIL_OPT_AGGRESSIVE) {
        anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                        "Invalid optimization level %d", (int)level);
        return ANVIL_ERR_INVALID_ARG;
    }
    
    pm->level = level;
    
    /* Enable/disable passes based on level */
    for (int i = 0; i < ANVIL_PASS_COUNT; i++) {
        pm->enabled[i] = builtin_passes[i].run != NULL &&
                         level >= builtin_passes[i].min_level;
    }
    return ANVIL_OK;
}

anvil_opt_level_t anvil_pass_manager_get_level(anvil_pass_manager_t *pm)
{
    return pm ? pm->level : ANVIL_OPT_NONE;
}

anvil_error_t anvil_pass_manager_set_iteration_limit(
    anvil_pass_manager_t *pm, unsigned limit)
{
    if (!pm || limit == 0) {
        if (pm && pm->ctx) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                            "Pass iteration limit must be greater than zero");
        }
        return ANVIL_ERR_INVALID_ARG;
    }
    pm->iteration_limit = limit;
    return ANVIL_OK;
}

unsigned anvil_pass_manager_get_iteration_limit(
    const anvil_pass_manager_t *pm)
{
    return pm ? pm->iteration_limit : 0;
}

anvil_error_t anvil_pass_manager_enable(anvil_pass_manager_t *pm,
                                         anvil_pass_id_t pass)
{
    if (!pm) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)pass >= (unsigned)ANVIL_PASS_COUNT ||
        builtin_passes[pass].run == NULL) {
        anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                        "Invalid optimization pass ID %d", (int)pass);
        return ANVIL_ERR_INVALID_ARG;
    }
    pm->enabled[pass] = true;
    return ANVIL_OK;
}

anvil_error_t anvil_pass_manager_disable(anvil_pass_manager_t *pm,
                                          anvil_pass_id_t pass)
{
    if (!pm) return ANVIL_ERR_INVALID_ARG;
    if ((unsigned)pass >= (unsigned)ANVIL_PASS_COUNT) {
        anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                        "Invalid optimization pass ID %d", (int)pass);
        return ANVIL_ERR_INVALID_ARG;
    }
    pm->enabled[pass] = false;
    return ANVIL_OK;
}

bool anvil_pass_manager_is_enabled(anvil_pass_manager_t *pm, anvil_pass_id_t pass)
{
    if (!pm || (unsigned)pass >= (unsigned)ANVIL_PASS_COUNT) return false;
    return pm->enabled[pass];
}

static anvil_pass_result_t run_one_pass(anvil_pass_manager_t *pm,
                                        anvil_func_t *func,
                                        const anvil_pass_info_t *pass)
{
    anvil_pass_result_t result = pass->run(func);
    if (anvil_ctx_get_last_error(pm->ctx) != ANVIL_OK) {
        return ANVIL_PASS_RUN_ERROR;
    }
    if (result != ANVIL_PASS_RUN_ERROR &&
        result != ANVIL_PASS_RUN_UNCHANGED &&
        result != ANVIL_PASS_RUN_CHANGED) {
        anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_OP,
                        "Optimization pass '%s' returned invalid status %d",
                        pass->name, (int)result);
        return ANVIL_PASS_RUN_ERROR;
    }
    if (result == ANVIL_PASS_RUN_ERROR) {
        if (anvil_ctx_get_last_error(pm->ctx) == ANVIL_OK) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_OP,
                            "Optimization pass '%s' (%s) failed",
                            pass->name, pass->description);
        }
        return result;
    }

    char verify_error[256] = { 0 };
    if (!anvil_func_verify(func, verify_error, sizeof(verify_error))) {
        anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_OP,
                        "Optimization pass '%s' produced invalid IR: %s",
                        pass->name, verify_error[0] ? verify_error : "invalid IR");
        return ANVIL_PASS_RUN_ERROR;
    }
    return result;
}

anvil_pass_result_t anvil_pass_manager_run_func(anvil_pass_manager_t *pm,
                                                 anvil_func_t *func)
{
    if (!pm || !func || !func->parent || func->parent->ctx != pm->ctx) {
        if (pm && pm->ctx) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                            "Pass manager/function context mismatch");
        }
        return ANVIL_PASS_RUN_ERROR;
    }
    anvil_ctx_clear_error(pm->ctx);
    if (func->is_declaration) return ANVIL_PASS_RUN_UNCHANGED;
    
    bool changed = false;
    bool any_changed;
    unsigned iterations = 0;
    const unsigned max_iterations = pm->iteration_limit;
    
    /* Run passes until fixpoint or max iterations */
    do {
        any_changed = false;
        
        /* Run built-in passes in the explicit ordering above. */
        for (int idx = 0; idx < ANVIL_PASS_COUNT; idx++) {
            int i = pass_exec_order[idx];
            if (pm->enabled[i] && builtin_passes[i].run) {
                anvil_pass_result_t result = run_one_pass(
                    pm, func, &builtin_passes[i]);
                if (result == ANVIL_PASS_RUN_ERROR)
                    return ANVIL_PASS_RUN_ERROR;
                if (result == ANVIL_PASS_RUN_CHANGED) {
                    any_changed = true;
                    changed = true;
                }
            }
        }
        
        /* Run custom passes */
        for (size_t i = 0; i < pm->num_custom; i++) {
            if (pm->level < pm->custom_passes[i].min_level) continue;
            anvil_pass_result_t result = run_one_pass(
                pm, func, &pm->custom_passes[i]);
            if (result == ANVIL_PASS_RUN_ERROR)
                return ANVIL_PASS_RUN_ERROR;
            if (result == ANVIL_PASS_RUN_CHANGED) {
                any_changed = true;
                changed = true;
            }
        }
        
        iterations++;
        if (any_changed && iterations == max_iterations) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_OP,
                            "Optimization pipeline did not converge after %u iterations",
                            max_iterations);
            return ANVIL_PASS_RUN_ERROR;
        }
    } while (any_changed);

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

anvil_pass_result_t anvil_pass_manager_run_module(anvil_pass_manager_t *pm,
                                                   anvil_module_t *mod)
{
    if (!pm || !mod || mod->ctx != pm->ctx) {
        if (pm && pm->ctx) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                            "Pass manager/module context mismatch");
        }
        return ANVIL_PASS_RUN_ERROR;
    }
    
    bool changed = false;
    
    /* Run passes on each function */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        anvil_pass_result_t result = anvil_pass_manager_run_func(pm, func);
        if (result == ANVIL_PASS_RUN_ERROR)
            return ANVIL_PASS_RUN_ERROR;
        if (result == ANVIL_PASS_RUN_CHANGED) {
            changed = true;
        }
    }

    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

anvil_error_t anvil_pass_manager_register(anvil_pass_manager_t *pm,
                                           const anvil_pass_info_t *pass)
{
    if (!pm || !pass || pass->id != ANVIL_PASS_CUSTOM || !pass->run ||
        !pass->name || !pass->name[0] || !pass->description ||
        (unsigned)pass->min_level > (unsigned)ANVIL_OPT_AGGRESSIVE) {
        if (pm && pm->ctx) {
            anvil_set_error(pm->ctx, ANVIL_ERR_INVALID_ARG,
                            "Invalid custom optimization pass descriptor");
        }
        return ANVIL_ERR_INVALID_ARG;
    }

    char *name = anvil_ctx_strdup(pm->ctx, pass->name);
    if (!name) return ANVIL_ERR_NOMEM;
    char *description = anvil_ctx_strdup(pm->ctx, pass->description);
    if (!description) {
        free(name);
        return ANVIL_ERR_NOMEM;
    }
    
    /* Grow array if needed */
    if (pm->num_custom >= pm->cap_custom) {
        if (pm->cap_custom > SIZE_MAX / 2) {
            free(name);
            free(description);
            anvil_set_error(pm->ctx, ANVIL_ERR_NOMEM,
                            "Custom pass table capacity overflow");
            return ANVIL_ERR_NOMEM;
        }
        size_t new_cap = pm->cap_custom ? pm->cap_custom * 2 : 4;
        if (new_cap > SIZE_MAX / sizeof(*pm->custom_passes)) {
            free(name);
            free(description);
            anvil_set_error(pm->ctx, ANVIL_ERR_NOMEM,
                            "Custom pass table size overflow");
            return ANVIL_ERR_NOMEM;
        }
        anvil_pass_info_t *new_passes = anvil_ctx_realloc(
            pm->ctx, pm->custom_passes,
            new_cap * sizeof(*pm->custom_passes));
        if (!new_passes) {
            free(name);
            free(description);
            return ANVIL_ERR_NOMEM;
        }
        pm->custom_passes = new_passes;
        pm->cap_custom = new_cap;
    }
    
    pm->custom_passes[pm->num_custom] = *pass;
    pm->custom_passes[pm->num_custom].name = name;
    pm->custom_passes[pm->num_custom].description = description;
    pm->num_custom++;
    return ANVIL_OK;
}
