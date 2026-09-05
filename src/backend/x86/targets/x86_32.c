#include "../x86_32_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const anvil_arch_info_t x86_arch_info = {
    .arch = ANVIL_ARCH_X86,
    .name = "x86",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 8,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t x86_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    x86_backend_priv_t *priv = calloc(1, sizeof(x86_backend_priv_t));
    if (!priv)
        return ANVIL_ERR_NOMEM;

    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->ctx = ctx;
    priv->syntax = ctx->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : ctx->syntax;

    be->priv = priv;
    return ANVIL_OK;
}

static void x86_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv)
        return;

    x86_backend_priv_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
}

static void x86_reset(anvil_backend_t *be)
{
    (void)be;
}

static const anvil_arch_info_t *x86_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &x86_arch_info;
}

const anvil_backend_ops_t anvil_backend_x86 = {
    .name = "x86",
    .arch = ANVIL_ARCH_X86,
    .init = x86_init,
    .cleanup = x86_cleanup,
    .reset = x86_reset,
    .codegen_module = x86_codegen_module,
    .codegen_func = x86_codegen_func,
    .get_arch_info = x86_get_arch_info
};
