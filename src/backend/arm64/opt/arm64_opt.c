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
    
    /* Run optimization passes in order */
    
    /* 1. Peephole optimizations - local improvements */
    arm64_opt_peephole(be, func);
    
    /* 2. Dead store elimination */
    arm64_opt_dead_store(be, func);
    
    /* 3. Redundant load elimination */
    arm64_opt_load_elim(be, func);
    
    /* 4. Branch/comparison optimization */
    arm64_opt_branch(be, func);
    
    /* 5. Immediate operand optimization */
    arm64_opt_immediate(be, func);
}
