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
    .has_delay_slots = false,
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

/* Words and smaller, with CMPXCHG/XADD (i486 and later). 64-bit atomics
 * would need CMPXCHG8B sequences, which the lowering does not produce. */
static bool x86_atomic_is_lock_free(anvil_backend_t *be, anvil_type_t *type)
{
    return anvil_atomic_type_valid(type) && type->size <= 4 && anvil_ctx_has_feature(be->ctx, ANVIL_FEATURE_X86_I486);
}

/* Position independence where the object format has GOT relocations
 * (the GOT pointer comes from call/pop). */
static bool x86_reloc_model_supported(anvil_backend_t *be, anvil_reloc_model_t model)
{
    const anvil_x86_plat_desc_t *plat = anvil_x86_get_plat_desc(be->ctx ? be->ctx->abi : ANVIL_ABI_DEFAULT);
    return plat && plat->format->got_relocations && (model == ANVIL_RELOC_PIE || model == ANVIL_RELOC_PIC);
}

const anvil_backend_ops_t anvil_backend_x86 = {
    .name = "x86",
    .arch = ANVIL_ARCH_X86,
    .init = x86_init,
    .cleanup = x86_cleanup,
    .reset = x86_reset,
    .codegen_module = x86_codegen_module,
    .codegen_func = x86_codegen_func,
    .get_arch_info = x86_get_arch_info,
    .build_va_arg = anvil_x86_build_va_arg,
    .build_va_copy = anvil_x86_build_va_copy,
    .build_va_copy_into = anvil_x86_build_va_copy_into,
    .param_attr_support = (1u << ANVIL_PARAM_BYVAL) | (1u << ANVIL_PARAM_SRET),
    .atomic_is_lock_free = x86_atomic_is_lock_free,
    .reloc_model_supported = x86_reloc_model_supported,
    .vector_operation_cost = x86_scalar_operation_cost,
    .abi_support = (1u << ANVIL_ABI_SYSV) | (1u << ANVIL_ABI_DARWIN) | (1u << ANVIL_ABI_WIN64)
};
