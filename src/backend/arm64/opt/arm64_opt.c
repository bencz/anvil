/*
 * ANVIL - ARM64 Backend Optimizations
 * 
 * Main optimization pass manager for ARM64-specific optimizations.
 */

#include "arm64_opt.h"
#include <string.h>

/* ============================================================================
 * Optimization Pass Manager
 * ============================================================================ */

void arm64_opt_module(arm64_backend_t *be, anvil_module_t *mod)
{
    if (!be || !mod) return;
    
    /* Run optimizations on each function */
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            arm64_opt_function(be, func);
        }
    }
}

void arm64_opt_function(arm64_backend_t *be, anvil_func_t *func)
{
    if (!be || !func) return;

    /* Run implemented ARM64-specific optimisation passes. Previously three
     * empty TODO stubs (dead_store, load_elim, immediate) were called on
     * every function for zero effect; they have been removed from the
     * pipeline until real implementations land. */

    /* 1. Peephole optimizations - local improvements */
    arm64_opt_peephole(be, func);

    /* 2. Branch/comparison optimization (cmp+cset+cbnz → b.cond, cbz/cbnz) */
    arm64_opt_branch(be, func);
}
