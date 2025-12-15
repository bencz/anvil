/*
 * ANVIL - Context Integration for Optimization
 * 
 * Integrates the optimization pass manager with the context API.
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>

/* Get or create pass manager for context */
anvil_pass_manager_t *anvil_ctx_get_pass_manager(anvil_ctx_t *ctx)
{
    if (!ctx) return NULL;
    
    /* Create pass manager if it doesn't exist */
    if (!ctx->pass_manager) {
        ctx->pass_manager = anvil_pass_manager_create(ctx);
        if (ctx->pass_manager) {
            anvil_pass_manager_set_level(ctx->pass_manager, (anvil_opt_level_t)ctx->opt_level);
        }
    }
    
    return ctx->pass_manager;
}

/* Set optimization level for context */
anvil_error_t anvil_ctx_set_opt_level(anvil_ctx_t *ctx, anvil_opt_level_t level)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    ctx->opt_level = (int)level;
    
    /* Update pass manager if it exists */
    if (ctx->pass_manager) {
        anvil_pass_manager_set_level(ctx->pass_manager, level);
    }
    
    return ANVIL_OK;
}

/* Get optimization level for context */
anvil_opt_level_t anvil_ctx_get_opt_level(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_OPT_NONE;
    return (anvil_opt_level_t)ctx->opt_level;
}

/* Run optimization passes on module */
anvil_error_t anvil_module_optimize(anvil_module_t *mod)
{
    if (!mod || !mod->ctx) return ANVIL_ERR_INVALID_ARG;
    
    /* Skip if no optimization enabled */
    if (mod->ctx->opt_level == ANVIL_OPT_NONE) {
        return ANVIL_OK;
    }
    
    anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(mod->ctx);
    if (!pm) return ANVIL_ERR_NOMEM;
    
    anvil_pass_manager_run_module(pm, mod);
    
    return ANVIL_OK;
}
