/*
 * ANVIL - Optimization Pass Infrastructure
 * 
 * Provides a framework for IR optimization passes that can be
 * enabled or disabled at compile time.
 */

#ifndef ANVIL_OPT_H
#define ANVIL_OPT_H

#include "anvil.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optimization levels */
typedef enum {
    ANVIL_OPT_NONE = 0,      /* No optimization (O0) */
    ANVIL_OPT_DEBUG = 1,     /* Debug-friendly optimizations (Og) - minimal IR cleanup */
    ANVIL_OPT_BASIC = 2,     /* Basic optimizations (O1) */
    ANVIL_OPT_STANDARD = 3,  /* Standard optimizations (O2) */
    ANVIL_OPT_AGGRESSIVE = 4 /* Aggressive optimizations (O3) */
} anvil_opt_level_t;

/* Individual optimization pass IDs */
typedef enum {
    ANVIL_PASS_CUSTOM = -1,      /* Sentinel required for custom passes */
    ANVIL_PASS_CONST_FOLD = 0,   /* Constant folding (O1+) */
    ANVIL_PASS_DCE,              /* Dead code elimination (O1+) */
    ANVIL_PASS_SIMPLIFY_CFG,     /* Simplify control flow graph (O2+) */
    ANVIL_PASS_STRENGTH_REDUCE,  /* Strength reduction (O2+) */
    ANVIL_PASS_COPY_PROP,        /* Copy propagation (Og+) */
    ANVIL_PASS_DEAD_STORE,       /* Dead store elimination (O2+) */
    ANVIL_PASS_LOAD_ELIM,        /* Redundant load elimination (O2+) */
    ANVIL_PASS_STORE_LOAD_PROP,  /* Store-load propagation (Og+) */
    ANVIL_PASS_COMMON_SUBEXPR,   /* Common subexpression elimination (O2+) */
    ANVIL_PASS_COUNT
} anvil_pass_id_t;

/* Pass manager handle */
typedef struct anvil_pass_manager anvil_pass_manager_t;

/* A pass must distinguish a valid no-change result from failure. */
typedef enum {
    ANVIL_PASS_RUN_ERROR = -1,
    ANVIL_PASS_RUN_UNCHANGED = 0,
    ANVIL_PASS_RUN_CHANGED = 1
} anvil_pass_result_t;

/* Custom-pass function signature. Built-in direct pass functions retain their
 * convenient bool (changed/no-change) API and are adapted by the manager. */
typedef anvil_pass_result_t (*anvil_pass_func_t)(anvil_func_t *func);

/* Pass information */
typedef struct {
    anvil_pass_id_t id;
    const char *name;
    const char *description;
    anvil_pass_func_t run;
    anvil_opt_level_t min_level;  /* Minimum opt level to enable this pass */
} anvil_pass_info_t;

/* ============================================================================
 * Pass Manager API
 * ============================================================================ */

/* Create a pass manager for a context */
anvil_pass_manager_t *anvil_pass_manager_create(anvil_ctx_t *ctx);

/* Destroy a pass manager */
void anvil_pass_manager_destroy(anvil_pass_manager_t *pm);

/* Set optimization level (enables/disables passes accordingly) */
anvil_error_t anvil_pass_manager_set_level(anvil_pass_manager_t *pm,
                                            anvil_opt_level_t level);

/* Get current optimization level */
anvil_opt_level_t anvil_pass_manager_get_level(anvil_pass_manager_t *pm);

/* Set/get the maximum number of fixpoint iterations. The default is 10.
 * A zero limit is invalid. */
anvil_error_t anvil_pass_manager_set_iteration_limit(
    anvil_pass_manager_t *pm, unsigned limit);
unsigned anvil_pass_manager_get_iteration_limit(
    const anvil_pass_manager_t *pm);

/* Enable a specific pass */
anvil_error_t anvil_pass_manager_enable(anvil_pass_manager_t *pm,
                                         anvil_pass_id_t pass);

/* Disable a specific pass */
anvil_error_t anvil_pass_manager_disable(anvil_pass_manager_t *pm,
                                          anvil_pass_id_t pass);

/* Check if a pass is enabled */
bool anvil_pass_manager_is_enabled(anvil_pass_manager_t *pm, anvil_pass_id_t pass);

/* Run all enabled passes. ERROR is also reflected in the context error state. */
anvil_pass_result_t anvil_pass_manager_run_func(anvil_pass_manager_t *pm,
                                                 anvil_func_t *func);

/* Run all enabled passes on a module */
anvil_pass_result_t anvil_pass_manager_run_module(anvil_pass_manager_t *pm,
                                                   anvil_module_t *mod);

/* Register a custom pass */
anvil_error_t anvil_pass_manager_register(anvil_pass_manager_t *pm, 
                                           const anvil_pass_info_t *pass);

/* ============================================================================
 * Context Integration API
 * ============================================================================ */

/* Set optimization level for context (creates pass manager if needed) */
anvil_error_t anvil_ctx_set_opt_level(anvil_ctx_t *ctx, anvil_opt_level_t level);

/* Get optimization level for context */
anvil_opt_level_t anvil_ctx_get_opt_level(anvil_ctx_t *ctx);

/* Get pass manager for context (creates if needed) */
anvil_pass_manager_t *anvil_ctx_get_pass_manager(anvil_ctx_t *ctx);

/* Run optimization passes on module before codegen */
anvil_error_t anvil_module_optimize(anvil_module_t *mod);

/* ============================================================================
 * Built-in Pass Functions (for direct use or custom pipelines)
 * ============================================================================ */

/* Direct built-ins use the same changed/unchanged/error contract as managed
 * and custom passes. */

/* Constant folding: evaluate constant expressions at compile time */
anvil_pass_result_t anvil_pass_const_fold(anvil_func_t *func);

/* Dead code elimination: remove unused instructions */
anvil_pass_result_t anvil_pass_dce(anvil_func_t *func);

/* Simplify CFG: merge blocks, remove unreachable code */
anvil_pass_result_t anvil_pass_simplify_cfg(anvil_func_t *func);

/* Strength reduction: replace expensive ops with cheaper ones */
anvil_pass_result_t anvil_pass_strength_reduce(anvil_func_t *func);

/* Copy propagation: replace uses of copied values with original */
anvil_pass_result_t anvil_pass_copy_prop(anvil_func_t *func);

/* Dead store elimination: remove stores that are overwritten before read */
anvil_pass_result_t anvil_pass_dead_store(anvil_func_t *func);

/* Redundant load elimination: reuse loaded values */
anvil_pass_result_t anvil_pass_load_elim(anvil_func_t *func);

/* Common subexpression elimination: reuse computed values */
anvil_pass_result_t anvil_pass_cse(anvil_func_t *func);

/* Store-load propagation: replace load after store with stored value */
anvil_pass_result_t anvil_pass_store_load_prop(anvil_func_t *func);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_OPT_H */
