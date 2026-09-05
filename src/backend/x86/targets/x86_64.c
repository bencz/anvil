#include "../x86_64_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const anvil_arch_info_t x64_arch_info = {
    .arch = ANVIL_ARCH_X86_64,
    .name = "x86-64",
    .ptr_size = 8,
    .addr_bits = 64,
    .word_size = 8,
    .num_gpr = 16,
    .num_fpr = 16,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t x64_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    x64_backend_t *priv = calloc(1, sizeof(x64_backend_t));
    if (!priv)
        return ANVIL_ERR_NOMEM;

    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->ctx = ctx;
    priv->syntax = ctx->syntax == ANVIL_SYNTAX_DEFAULT ? ANVIL_SYNTAX_GAS : ctx->syntax;

    be->priv = priv;
    return ANVIL_OK;
}

static void x64_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv)
        return;

    x64_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv->strings);
    free(priv);
    be->priv = NULL;
}

static void x64_reset(anvil_backend_t *be)
{
    if (!be || !be->priv)
        return;

    x64_backend_t *priv = be->priv;
    priv->num_strings = 0;
    priv->string_counter = 0;
}

static const anvil_arch_info_t *x64_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &x64_arch_info;
}

static bool x64_atomic_is_lock_free(anvil_backend_t *be, anvil_type_t *type)
{
    (void)be;
    return anvil_atomic_type_valid(type);
}

static unsigned x64_vector_operation_cost(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type)
{
    if (type->kind != ANVIL_TYPE_VECTOR || type->size != 16 || !anvil_ctx_has_feature(be->ctx, ANVIL_FEATURE_X86_SSE2))
        return 0;

    switch (operation) {
    case ANVIL_OP_LOAD:
    case ANVIL_OP_STORE:
    case ANVIL_OP_FADD:
    case ANVIL_OP_FSUB:
    case ANVIL_OP_FMUL:
        return 1;
    case ANVIL_OP_FDIV:
        return 4;
    default:
        return 0;
    }
}

const anvil_backend_ops_t anvil_backend_x86_64 = {
    .name = "x86-64",
    .arch = ANVIL_ARCH_X86_64,
    .init = x64_init,
    .cleanup = x64_cleanup,
    .reset = x64_reset,
    .codegen_module = x64_codegen_module,
    .codegen_func = x64_codegen_func,
    .get_arch_info = x64_get_arch_info,
    .classify_abi_value = x64_classify_abi_value,
    .build_va_arg = anvil_x64_build_va_arg,
    .build_va_copy = anvil_x64_build_va_copy,
    .build_va_copy_into = anvil_x64_build_va_copy_into,
    .atomic_is_lock_free = x64_atomic_is_lock_free,
    .vector_operation_cost = x64_vector_operation_cost
};
